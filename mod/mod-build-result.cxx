// file      : mod/mod-build-result.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-build-result.hxx>

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <libbutl/sendmail.hxx>
#include <libbutl/fdstream.hxx>
#include <libbutl/process-io.hxx>
#include <libbutl/manifest-parser.hxx>
#include <libbutl/manifest-serializer.hxx>
#include <libbutl/semantic-version.hxx>

#include <libbbot/manifest.hxx>

#include <web/server/module.hxx>

#include <libbrep/build.hxx>
#include <libbrep/build-odb.hxx>
#include <libbrep/package.hxx>
#include <libbrep/package-odb.hxx>

#include <mod/build.hxx>          // *_url()
#include <mod/module-options.hxx>

using namespace std;
using namespace butl;
using namespace bbot;
using namespace brep::cli;
using namespace odb::core;

// While currently the user-defined copy constructor is not required (we don't
// need to deep copy nullptr's), it is a good idea to keep the placeholder
// ready for less trivial cases.
//
brep::build_result::
build_result (const build_result& r)
    : build_result_module (r),
      options_ (r.initialized_ ? r.options_  : nullptr)
{
}

void brep::build_result::
init (scanner& s)
{
  HANDLER_DIAG;

  options_ = make_shared<options::build_result> (
    s, unknown_mode::fail, unknown_mode::fail);

  if (options_->build_config_specified ())
  {
    build_result_module::init (*options_, *options_);

    database_module::init (static_cast<const options::package_db&> (*options_),
                           options_->package_db_retry ());
  }

  if (options_->root ().empty ())
    options_->root (dir_path ("/"));
}

bool brep::build_result::
handle (request& rq, response&)
{
  using brep::version; // Not to confuse with module::version.

  HANDLER_DIAG;

  if (build_db_ == nullptr)
    throw invalid_request (501, "not implemented");

  // Make sure no parameters passed.
  //
  try
  {
    // Note that we expect the result request manifest to be posted and so
    // consider parameters from the URL only.
    //
    name_value_scanner s (rq.parameters (0 /* limit */, true /* url_only */));
    params::build_result (s, unknown_mode::fail, unknown_mode::fail);
  }
  catch (const cli::exception& e)
  {
    throw invalid_request (400, e.what ());
  }

  result_request_manifest rqm;

  try
  {
    // We fully cache the request content to be able to retry the request
    // handling if odb::recoverable is thrown (see database-module.cxx for
    // details).
    //
    size_t limit (options_->build_result_request_max_size ());
    manifest_parser p (rq.content (limit, limit), "result_request_manifest");
    rqm = result_request_manifest (p);
  }
  catch (const manifest_parsing& e)
  {
    throw invalid_request (400, e.what ());
  }

  // Parse the task response session and make sure the session matches tenant
  // and the result manifest's package name, and version.
  //
  parse_session_result session;
  const build_id& id (session.id);

  try
  {
    // Note: also verifies that the tenant matches the session.
    //
    session = parse_session (rqm.session);

    if (rqm.result.name != id.package.name)
      throw invalid_argument ("package name mismatch");

    if (rqm.result.version != session.package_version)
      throw invalid_argument ("package version mismatch");
  }
  catch (const invalid_argument& e)
  {
    throw invalid_request (400, string ("invalid session: ") + e.what ());
  }

  // If the session expired (no such configuration, package, etc), then we log
  // this case with the warning severity and respond with the 200 HTTP code as
  // if the session is valid. The thinking is that this is a problem with the
  // controller's setup (expires too fast), not with the agent's.
  //
  auto warn_expired = [&rqm, &warn] (const string& d)
  {
    warn << "session '" << rqm.session << "' expired: " << d;
  };

  // Make sure the build configuration still exists.
  //
  const build_target_config* tc;
  {
    auto i (target_conf_map_->find (
              build_target_config_id {id.target, id.target_config_name}));

    if (i == target_conf_map_->end ())
    {
      warn_expired ("no build configuration");
      return true;
    }

    tc = i->second;
  }

  // Load the built package (if present).
  //
  // The only way not to deal with 2 databases simultaneously is to pull
  // another bunch of the package fields into the build_package foreign
  // object, which is a pain (see build_package.hxx for details). Doesn't seem
  // worth it here: email members are really secondary and we don't need to
  // switch transactions back and forth.
  //
  shared_ptr<package> pkg;
  {
    transaction t (package_db_->begin ());
    pkg = package_db_->find<package> (id.package);
    t.commit ();
  }

  if (pkg == nullptr)
  {
    warn_expired ("no package");
    return true;
  }

  auto print_args = [&trace, this] (const char* args[], size_t n)
  {
    l2 ([&]{trace << process_args {args, n};});
  };

  // Load and update the package build configuration (if present).
  //
  // NULL if the package build doesn't exist or is not updated for any reason
  // (authentication failed, etc) or the configuration is excluded by the
  // package.
  //
  shared_ptr<build> bld;

  bool build_notify (false);
  bool unforced (true);

  // Note that if the session authentication fails (probably due to the
  // authentication settings change), then we log this case with the warning
  // severity and respond with the 200 HTTP code as if the challenge is
  // valid. The thinking is that we shouldn't alarm a law-abaiding agent and
  // shouldn't provide any information to a malicious one.
  //
  {
    transaction t (build_db_->begin ());

    package_build pb;
    shared_ptr<build> b;
    if (!build_db_->query_one<package_build> (
          query<package_build>::build::id == id, pb))
    {
      warn_expired ("no package build");
    }
    else if ((b = move (pb.build))->state != build_state::building)
    {
      warn_expired ("package configuration state is " + to_string (b->state));
    }
    else if (b->timestamp != session.timestamp)
    {
      warn_expired ("non-matching timestamp");
    }
    else if (authenticate_session (*options_, rqm.challenge, *b, rqm.session))
    {
      // If the build is interrupted, then revert it to the original built
      // state if this is a rebuild and delete it from the database otherwise.
      //
      if (rqm.result.status == result_status::interrupt)
      {
        if (b->status) // Is this a rebuild?
        {
          b->state = build_state::built;

          // Keep the force rebuild indication. Note that the forcing state is
          // only valid for the building state.
          //
          if (b->force == force_state::forcing)
            b->force = force_state::forced;

          // Cleanup the interactive build login information.
          //
          b->interactive = nullopt;

          // Cleanup the authentication data.
          //
          b->agent_fingerprint = nullopt;
          b->agent_challenge = nullopt;

          // Note that we are unable to restore the pre-rebuild timestamp
          // since it has been overwritten when the build task was issued.
          // That, however, feels ok and we just keep it unchanged.

          build_db_->update (b);
        }
        else
          build_db_->erase (b);
      }
      else
      {
        // Verify the result status/checksums.
        //
        // Specifically, if the result status is skip, then it can only be in
        // response to the soft rebuild task (all checksums are present in the
        // build object) and the result checksums must match the build object
        // checksums. On verification failure respond with the bad request
        // HTTP code (400).
        //
        if (rqm.result.status == result_status::skip)
        {
          if (!b->agent_checksum  ||
              !b->worker_checksum ||
              !b->dependency_checksum)
            throw invalid_request (400, "unexpected skip result status");

          // Can only be absent for initial build, in which case the
          // checksums are also absent and we would end up with the above
          // 400 response.
          //
          assert (b->status);

          // Verify that the result checksum matches the build checksum and
          // throw invalid_request(400) if that's not the case.
          //
          auto verify = [] (const string& build_checksum,
                            const optional<string>& result_checksum,
                            const char* what)
          {
            if (!result_checksum)
              throw invalid_request (
                400,
                string (what) + " checksum is expected for skip result status");

            if (*result_checksum != build_checksum)
              throw invalid_request (
                400,
                string (what) + " checksum '" + build_checksum  +
                "' is expected instead of '" + *result_checksum +
                "' for skip result status");
          };

          verify (*b->agent_checksum, rqm.agent_checksum, "agent");

          verify (*b->worker_checksum,
                  rqm.result.worker_checksum,
                  "worker");

          verify (*b->dependency_checksum,
                  rqm.result.dependency_checksum,
                  "dependency");
        }

        unforced = b->force == force_state::unforced;

        // Don't send email to the build-email address for the
        // success-to-success status change, unless the build was forced.
        //
        build_notify = !(rqm.result.status == result_status::success &&
                         b->status                                   &&
                         *b->status == rqm.result.status             &&
                         unforced);

        b->state  = build_state::built;
        b->force  = force_state::unforced;

        // Cleanup the interactive build login information.
        //
        b->interactive = nullopt;

        // Cleanup the authentication data.
        //
        b->agent_fingerprint = nullopt;
        b->agent_challenge = nullopt;

        b->timestamp = system_clock::now ();
        b->soft_timestamp = b->timestamp;

        // If the result status is other than skip, then save the status,
        // results, and checksums and update the hard timestamp.
        //
        if (rqm.result.status != result_status::skip)
        {
          b->status = rqm.result.status;
          b->hard_timestamp = b->soft_timestamp;

          // Mark the section as loaded, so results are updated.
          //
          b->results_section.load ();
          b->results = move (rqm.result.results);

          // Save the checksums.
          //
          b->agent_checksum      = move (rqm.agent_checksum);
          b->worker_checksum     = move (rqm.result.worker_checksum);
          b->dependency_checksum = move (rqm.result.dependency_checksum);
        }

        build_db_->update (b);

        // Don't send the build notification email if the task result is
        // `skip`, the configuration is hidden, or is now excluded by the
        // package.
        //
        if (rqm.result.status != result_status::skip && belongs (*tc, "all"))
        {
          shared_ptr<build_package> p (
            build_db_->load<build_package> (b->id.package));

          // The package configuration should be present (see mod-builds.cxx
          // for details) but if it is not, let's log the warning.
          //
          if (const build_package_config* pc = find (b->package_config_name,
                                                     p->configs))
          {
            build_db_->load (*p, p->constraints_section);

            if (!exclude (*pc, p->builds, p->constraints, *tc))
              bld = move (b);
          }
          else
            warn << "cannot find configuration '" << b->package_config_name
                 << "' for package " << p->id.name << '/' << p->version;
        }
      }
    }

    t.commit ();
  }

  if (bld == nullptr)
    return true;

  // Bail out if sending build notification emails is disabled for this
  // toolchain.
  //
  {
    const map<string, bool>& tes (options_->build_toolchain_email ());

    auto i (tes.find (bld->id.toolchain_name));
    if (i != tes.end () && !i->second)
      return true;
  }

  string subj ((unforced ? "build " : "rebuild ")   +
               to_string (*bld->status) + ": "      +
               bld->package_name.string () + '/'    +
               bld->package_version.string () + ' ' +
               bld->target_config_name + '/'        +
               bld->target.string () + ' '          +
               bld->package_config_name + ' '       +
               bld->toolchain_name + '-' + bld->toolchain_version.string ());

  // Send notification emails to the interested parties.
  //
  auto send_email = [&bld, &subj, &error, &trace, &print_args, this]
                    (const string& to)
  {
    try
    {
      l2 ([&]{trace << "email '" << subj << "' to " << to;});

      // Redirect the diagnostics to webserver error log.
      //
      // Note: if using this somewhere else, then need to factor out all this
      // exit status handling code.
      //
      sendmail sm (print_args,
                   2,
                   options_->email (),
                   subj,
                   {to});

      if (bld->results.empty ())
        sm.out << "No operation results available." << endl;
      else
      {
        const string& host (options_->host ());
        const dir_path& root (options_->root ());

        ostream& os (sm.out);

        assert (bld->status);
        os << "combined: " << *bld->status << endl << endl
           << "  " << build_log_url (host, root, *bld) << endl << endl;

        for (const auto& r: bld->results)
          os << r.operation << ": " << r.status << endl << endl
             << "  " << build_log_url (host, root, *bld, &r.operation)
             << endl << endl;

        os << "Force rebuild (enter the reason, use '+' instead of spaces):"
           << endl << endl
           << "  " << build_force_url (host, root, *bld) << endl;
      }

      sm.out.close ();

      if (!sm.wait ())
        error << "sendmail " << *sm.exit;
    }
    // Handle process_error and io_error (both derive from system_error).
    //
    catch (const system_error& e)
    {
      error << "sendmail error: " << e;
    }
  };

  // Send the build notification email if a non-empty package build email is
  // specified.
  //
  optional<email>& build_email (pkg->build_email);
  if (build_notify && build_email && !build_email->empty ())
    send_email (*pkg->build_email);

  assert (bld->status);

  // Send the build warning/error notification emails, if requested.
  //
  if (pkg->build_warning_email && *bld->status >= result_status::warning)
    send_email (*pkg->build_warning_email);

  if (pkg->build_error_email && *bld->status >= result_status::error)
    send_email (*pkg->build_error_email);

  return true;
}
