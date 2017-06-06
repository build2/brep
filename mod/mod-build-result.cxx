// file      : mod/mod-build-result.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-build-result.hxx>

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <libbutl/openssl.hxx>
#include <libbutl/sendmail.hxx>
#include <libbutl/fdstream.hxx>
#include <libbutl/process-io.hxx>
#include <libbutl/manifest-parser.hxx>
#include <libbutl/manifest-serializer.hxx>

#include <libbbot/manifest.hxx>

#include <web/module.hxx>

#include <libbrep/build.hxx>
#include <libbrep/build-odb.hxx>
#include <libbrep/package.hxx>
#include <libbrep/package-odb.hxx>

#include <mod/options.hxx>
#include <mod/build-config.hxx> // *_url()

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
    : database_module (r),
      options_ (r.initialized_ ? r.options_ : nullptr)
{
}

void brep::build_result::
init (scanner& s)
{
  MODULE_DIAG;

  options_ = make_shared<options::build_result> (
    s, unknown_mode::fail, unknown_mode::fail);

  database_module::init (static_cast<options::package_db> (*options_),
                         options_->package_db_retry ());

  if (options_->build_config_specified ())
    database_module::init (static_cast<options::build>    (*options_),
                           static_cast<options::build_db> (*options_),
                           options_->build_db_retry ());

  if (options_->root ().empty ())
    options_->root (dir_path ("/"));
}

bool brep::build_result::
handle (request& rq, response&)
{
  using brep::version; // Not to confuse with module::version.

  MODULE_DIAG;

  if (build_db_ == nullptr)
    throw invalid_request (501, "not implemented");

  // Make sure no parameters passed.
  //
  try
  {
    name_value_scanner s (rq.parameters ());
    params::build_result (s, unknown_mode::fail, unknown_mode::fail);
  }
  catch (const cli::exception& e)
  {
    throw invalid_request (400, e.what ());
  }

  result_request_manifest rqm;

  try
  {
    size_t limit (options_->build_result_request_max_size ());
    manifest_parser p (rq.content (limit, limit), "result_request_manifest");
    rqm = result_request_manifest (p);
  }
  catch (const manifest_parsing& e)
  {
    throw invalid_request (400, e.what ());
  }

  // Parse the task response session to obtain the build configuration name and
  // the timestamp, and to make sure the session matches the result manifest's
  // package name and version.
  //
  build_id id;
  timestamp session_timestamp;

  try
  {
    const string& s (rqm.session);

    size_t p (s.find ('/')); // End of package name.

    if (p == 0)
      throw invalid_argument ("empty package name");

    if (p == string::npos)
      throw invalid_argument ("no package version");

    string& name (rqm.result.name);
    if (name.compare (0, name.size (), s, 0, p) != 0)
      throw invalid_argument ("package name mismatch");

    size_t b (p + 1);    // Start of version.
    p = s.find ('/', b); // End of version.

    if (p == string::npos)
      throw invalid_argument ("no configuration name");

    auto parse_version = [&s, &b, &p] (const char* what) -> version
    {
      // Intercept exception handling to add the parsing error attribution.
      //
      try
      {
        return brep::version (string (s, b, p - b));
      }
      catch (const invalid_argument& e)
      {
        throw invalid_argument (string ("invalid ") + what + ": " + e.what ());
      }
    };

    version package_version (parse_version ("package version"));

    if (package_version != rqm.result.version)
      throw invalid_argument ("package version mismatch");

    b = p + 1;           // Start of configuration name.
    p = s.find ('/', b); // End of configuration name.

    if (p == string::npos)
      throw invalid_argument ("no toolchain version");

    string config (s, b, p - b);

    b = p + 1;           // Start of toolchain version.
    p = s.find ('/', b); // End of toolchain version.

    if (p == string::npos)
      throw invalid_argument ("no timestamp");

    version toolchain_version (parse_version ("toolchain version"));

    id = build_id (package_id (move (name), package_version),
                   move (config),
                   toolchain_version);

    if (id.configuration.empty ())
      throw invalid_argument ("empty configuration name");

    try
    {
      size_t tsn;
      string ts (s, p + 1);

      session_timestamp = timestamp (
        chrono::duration_cast<timestamp::duration> (
          chrono::nanoseconds (stoull (ts, &tsn))));

      if (tsn != ts.size ())
        throw invalid_argument ("trailing junk");
    }
    // Handle invalid_argument or out_of_range (both derive from logic_error),
    // that can be thrown by stoull().
    //
    catch (const logic_error& e)
    {
      throw invalid_argument (string ("invalid timestamp: ") + e.what ());
    }
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
  if (build_conf_map_->find (id.configuration.c_str ()) ==
      build_conf_map_->end ())
  {
    warn_expired ("no build configuration");
    return true;
  }

  // Load the built package (if present).
  //
  shared_ptr<package> p;
  {
    transaction t (package_db_->begin ());
    p = package_db_->find<package> (id.package);
    t.commit ();
  }

  if (p == nullptr)
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
  shared_ptr<build> b;
  optional<result_status> prev_status;
  bool notify (false);
  bool unforced (true);

  {
    transaction t (build_db_->begin ());
    b = build_db_->find<build> (id);

    if (b == nullptr)
      warn_expired ("no package configuration");
    else if (b->state != build_state::building)
      warn_expired ("package configuration state is " + to_string (b->state));
    else if (b->timestamp != session_timestamp)
      warn_expired ("non-matching timestamp");
    else
    {
      // Check the challenge.
      //
      // If the challenge doesn't match expectations (probably due to the
      // authentication settings change), then we log this case with the
      // warning severity and respond with the 200 HTTP code as if the
      // challenge is valid. The thinking is that we shouldn't alarm a
      // law-abaiding agent and shouldn't provide any information to a
      // malicious one.
      //
      auto warn_auth = [&rqm, &warn] (const string& d)
      {
        warn << "session '" << rqm.session << "' authentication failed: " << d;
      };

      bool auth (false);

      // Must both be present or absent.
      //
      if (!b->agent_challenge != !rqm.challenge)
        warn_auth (rqm.challenge
                   ? "unexpected challenge"
                   : "challenge is expected");
      else if (bot_agent_keys_ == nullptr) // Authentication is disabled.
        auth = true;
      else if (!b->agent_challenge) // Authentication is recently enabled.
        warn_auth ("challenge is required now");
      else
      {
        assert (b->agent_fingerprint && rqm.challenge);
        auto i (bot_agent_keys_->find (*b->agent_fingerprint));

        // The agent's key is recently replaced.
        //
        if (i == bot_agent_keys_->end ())
          warn_auth ("agent's public key not found");
        else
        {
          try
          {
            openssl os (print_args,
                        path ("-"), fdstream_mode::text, 2,
                        process_env (options_->openssl (),
                                     options_->openssl_envvar ()),
                        "rsautl",
                        options_->openssl_option (),
                        "-verify", "-pubin", "-inkey", i->second);

            for (const auto& c: *rqm.challenge)
              os.out.put (c); // Sets badbit on failure.

            os.out.close ();

            string s;
            getline (os.in, s);

            bool v (os.in.eof ());
            os.in.close ();

            if (os.wait () && v)
            {
              auth = s == *b->agent_challenge;

              if (!auth)
                warn_auth ("challenge mismatched");
            }
            else // The signature is presumably meaningless.
              warn_auth ("unable to verify challenge");
          }
          catch (const system_error& e)
          {
            fail << "unable to verify challenge: " << e;
          }
        }
      }

      if (auth)
      {
        unforced = b->force == force_state::unforced;

        // Don's send email for the success-to-success status change, unless
        // the build was forced.
        //
        notify = !(rqm.result.status == result_status::success &&
                   b->status && *b->status == rqm.result.status && unforced);

        prev_status = move (b->status);

        b->state  = build_state::built;
        b->status = rqm.result.status;
        b->force  = force_state::unforced;

        // Cleanup the authentication data.
        //
        b->agent_fingerprint = nullopt;
        b->agent_challenge = nullopt;

        // Mark the section as loaded, so results are updated.
        //
        b->results_section.load ();
        b->results = move (rqm.result.results);

        b->timestamp = timestamp::clock::now ();

        build_db_->update (b);
      }
    }

    t.commit ();
  }

  // Don't send the notification email if the empty package build email is
  // specified.
  //
  const optional<email>& build_email (p->build_email);
  if (!notify || (build_email && build_email->empty ()))
    return true;

  assert (b != nullptr);

  // Send email to the package owner.
  //
  try
  {
    string subj ((unforced ? "build " : "rebuild ") +
                 to_string (*b->status) + ": " + b->package_name + '/' +
                 b->package_version.string () + '/' + b->configuration + '/' +
                 b->toolchain_name + '-' + b->toolchain_version.string ());

    // If the package build address is not specified, then it is assumed to be
    // the same as the package email address, if specified, otherwise as the
    // project email address.
    //
    const string& to (build_email ? *build_email
                      : p->package_email
                        ? *p->package_email
                        : p->email);

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

    if (b->results.empty ())
      sm.out << "No operations results available." << endl;
    else
    {
      const string& host (options_->host ());
      const dir_path& root (options_->root ());

      ostream& os (sm.out);

      assert (b->status);
      os << "combined: " << *b->status << endl << endl
         << "  " << build_log_url (host, root, *b) << endl << endl;

      for (const auto& r: b->results)
        os << r.operation << ": " << r.status << endl << endl
           << "  " << build_log_url (host, root, *b, &r.operation)
           << endl << endl;

      os << "Force rebuild (enter the reason, use '+' instead of spaces):"
         << endl << endl
         << "  " << force_rebuild_url (host, root, *b) << endl;
    }

    sm.out.close ();

    if (!sm.wait ())
    {
      diag_record dr (error);
      dr << "sendmail ";

      assert (sm.exit);
      const process_exit& e (*sm.exit);

      if (e.normal ())
        dr << "exited with code " << static_cast<uint16_t> (e.code ());
      else
      {
        dr << "terminated abnormally: " << e.description ();

        if (e.core ())
          dr << " (core dumped)";
      }
    }
  }
  // Handle process_error and io_error (both derive from system_error).
  //
  catch (const system_error& e)
  {
    error << "sendmail error: " << e;
  }

  return true;
}
