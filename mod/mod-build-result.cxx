// file      : mod/mod-build-result.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-build-result.hxx>

#include <chrono>

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <libbutl/manifest-parser.hxx>
#include <libbutl/manifest-serializer.hxx>

#include <libbbot/manifest.hxx>

#include <web/server/module.hxx>

#include <libbrep/build.hxx>
#include <libbrep/build-odb.hxx>
#include <libbrep/build-package.hxx>
#include <libbrep/build-package-odb.hxx>

#include <mod/build.hxx>          // send_notification_email()
#include <mod/module-options.hxx>
#include <mod/tenant-service.hxx>

using namespace std;
using namespace butl;
using namespace bbot;
using namespace brep::cli;
using namespace odb::core;

brep::build_result::
build_result (const tenant_service_map& tsm)
    : tenant_service_map_ (tsm)
{
}

// While currently the user-defined copy constructor is not required (we don't
// need to deep copy nullptr's), it is a good idea to keep the placeholder
// ready for less trivial cases.
//
brep::build_result::
build_result (const build_result& r, const tenant_service_map& tsm)
    : build_result_module (r),
      options_ (r.initialized_ ? r.options_  : nullptr),
      tenant_service_map_ (tsm)
{
}

void brep::build_result::
init (scanner& s)
{
  options_ = make_shared<options::build_result> (
    s, unknown_mode::fail, unknown_mode::fail);

  if (options_->build_config_specified ())
    build_result_module::init (*options_, *options_);

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
  // Note, though, that there can be quite a common situation when a build
  // machine is suspended by the bbot agent due to the build timeout. In this
  // case the task result request may arrive anytime later (after the issue is
  // investigated, etc) with the abort or abnormal status. By that arrival
  // time a new build task may already be issued/completed for this package
  // build configuration or this configuration may even be gone (brep has been
  // reconfigured, package has gone, etc). We will log no warning in this
  // case, assuming that such an expiration is not a problem with the
  // controller's setup.
  //
  shared_ptr<build> b;
  result_status rs (rqm.result.status);

  auto warn_expired = [&rqm, &warn, &b, &session, rs] (const string& d)
  {
    if (!((b == nullptr || b->timestamp > session.timestamp) &&
          (rs == result_status::abort || rs == result_status::abnormal)))
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

  // Load and update the package build configuration (if present).
  //
  // NULL if the package build doesn't exist or is not updated for any reason
  // (authentication failed, etc) or the configuration is excluded by the
  // package.
  //
  shared_ptr<build> bld;

  // The built package configuration.
  //
  // Not NULL if bld is not NULL.
  //
  shared_ptr<build_package> pkg;
  build_package_config* cfg (nullptr);

  // Don't send email to the build-email address for the success-to-success
  // status change, unless the build was forced.
  //
  bool build_notify (false);
  bool unforced (true);

  // If the package is built (result status differs from interrupt, etc) and
  // the package tenant has a third-party service state associated with it,
  // then check if the tenant_service_build_built callback is registered for
  // the type of the associated service. If it is, then stash the state, the
  // build object, and the callback pointer for the subsequent service `built`
  // notification. Note that we send this notification for the skip result as
  // well, since it is semantically equivalent to the previous build result
  // with the actual build process being optimized out.
  //
  // If the package build is interrupted and the tenant_service_build_queued
  // callback is associated with the package tenant, then stash the state, the
  // build object, and the callback pointer and calculate the hints for the
  // subsequent service `queued` notification.
  //
  const tenant_service_build_built* tsb (nullptr);
  const tenant_service_build_queued* tsq (nullptr);
  optional<pair<tenant_service, shared_ptr<build>>> tss;
  tenant_service_build_queued::build_queued_hints qhs;

  // Acquire the database connection for the subsequent transactions.
  //
  // Note that we will release it prior to any potentially time-consuming
  // operations (such as HTTP requests) and re-acquire it again afterwards,
  // if required.
  //
  connection_ptr conn (build_db_->connection ());

  // Note that if the session authentication fails (probably due to the
  // authentication settings change), then we log this case with the warning
  // severity and respond with the 200 HTTP code as if the challenge is
  // valid. The thinking is that we shouldn't alarm a law-abaiding agent and
  // shouldn't provide any information to a malicious one.
  //
  {
    transaction t (conn->begin ());

    package_build pb;

    auto build_timestamp = [&b] ()
    {
      return to_string (
        chrono::duration_cast<std::chrono::nanoseconds> (
          b->timestamp.time_since_epoch ()).count ());
    };

    if (!build_db_->query_one<package_build> (
          query<package_build>::build::id == id, pb))
    {
      warn_expired ("no package build");
    }
    else if ((b = move (pb.build))->state != build_state::building)
    {
      warn_expired ("package configuration state is " + to_string (b->state) +
                    ", force state " + to_string (b->force)                  +
                    ", timestamp " + build_timestamp ());
    }
    else if (b->timestamp != session.timestamp)
    {
      warn_expired ("non-matching timestamp " + build_timestamp ());
    }
    else if (authenticate_session (*options_, rqm.challenge, *b, rqm.session))
    {
      // If the build is not in the `forcing` state, then retrieve the tenant
      // service callback, if present, for subsequent notification (`queued`
      // for the interrupted build and `built` otherwise; see below). Note
      // that for the `forcing` state the service already assumes the `queued`
      // state (see build_force::handle() and ci_start::rebuild() for
      // details).
      //
      const tenant_service_base* ts (nullptr);
      shared_ptr<build_tenant> t;

      if (b->force != force_state::forcing)
      {
        t = build_db_->load<build_tenant> (b->tenant);

        if (t->service)
        {
          auto i (tenant_service_map_.find (t->service->type));

          if (i != tenant_service_map_.end ())
            ts = i->second.get ();
        }
      }

      // If the build is interrupted, then revert it to the original built
      // state if this is a rebuild. Otherwise (initial build), turn the build
      // into the queued state if the tenant_service_build_queued callback is
      // registered for the package tenant and delete it from the database
      // otherwise.
      //
      // Note that if the tenant_service_build_queued callback is registered,
      // we always send the `queued` notification for the interrupted build,
      // even when we reverse it to the original built state. We could also
      // turn the build into the queued state in this case, but it feels that
      // there is no harm in keeping the previous build information available
      // for the user.
      //
      if (rs == result_status::interrupt)
      {
        // Schedule the `queued` notification, if the
        // tenant_service_build_queued callback is registered for the tenant.
        //
        tsq = dynamic_cast<const tenant_service_build_queued*> (ts);

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
          //
          // Moreover, we actually use the fact that the build's timestamp is
          // greater then its soft_timestamp as an indication that the build
          // object represents the interrupted rebuild (see the build_task
          // handler for details).
          //
          // @@ Actually, we also unable to restore the pre-rebuild machine
          //    and auxiliary machines, which are also displayed in the build
          //    log and may potentially be confusing. Should we drop them from
          //    the log in this case or replace with the "machine: unknown"
          //    record?

          build_db_->update (b);
        }
        else           // Initial build.
        {
          if (tsq != nullptr)
          {
            // Since this is not a rebuild, there are no operation results and
            // thus we don't need to load the results section to erase results
            // from the database.
            //
            assert (b->results.empty ());

            *b = build (move (b->tenant),
                        move (b->package_name),
                        move (b->package_version),
                        move (b->target),
                        move (b->target_config_name),
                        move (b->package_config_name),
                        move (b->toolchain_name),
                        move (b->toolchain_version));

            build_db_->update (b);
          }
          else
            build_db_->erase (b);
        }

        // If we ought to call the tenant_service_build_queued::build_queued()
        // callback, then also set the package tenant's queued timestamp to
        // prevent the task handler from picking the build and potentially
        // interfering with us by sending its `building` notification before
        // we send our `queued` notification (see tenant::queued_timestamp for
        // details).
        //
        if (tsq != nullptr)
        {
          assert (t != nullptr);

          // Calculate the tenant service hints.
          //
          buildable_package_count tpc (
            build_db_->query_value<buildable_package_count> (
              query<buildable_package_count>::build_tenant::id == t->id));

          shared_ptr<build_package> p (
            build_db_->load<build_package> (b->id.package));

          qhs = tenant_service_build_queued::build_queued_hints {
            tpc == 1, p->configs.size () == 1};

          // Set the package tenant's queued timestamp, unless it is already
          // set to the same or greater value.
          //
          timestamp ts (
            system_clock::now () +
            chrono::seconds (options_->build_queued_timeout ()));

          if (!t->queued_timestamp || *t->queued_timestamp < ts)
          {
            t->queued_timestamp = ts;
            build_db_->update (t);
          }
        }
      }
      else // Regular or skip build result.
      {
        // Schedule the `built` notification, if the
        // tenant_service_build_built callback is registered for the tenant.
        //
        tsb = dynamic_cast<const tenant_service_build_built*> (ts);

        // Verify the result status/checksums.
        //
        // Specifically, if the result status is skip, then it can only be in
        // response to the soft rebuild task (all checksums are present in the
        // build object) and the result checksums must match the build object
        // checksums. On verification failure respond with the bad request
        // HTTP code (400).
        //
        if (rs == result_status::skip)
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

        unforced = (b->force == force_state::unforced);

        build_notify = !(rs == result_status::success &&
                         b->status                    &&
                         *b->status == rs             &&
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
        // results, and checksums and update the hard timestamp. Also stash
        // the service notification information, if present.
        //
        if (rs != result_status::skip)
        {
          b->status = rs;
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

        pkg = build_db_->load<build_package> (b->id.package);
        cfg = find (b->package_config_name, pkg->configs);

        // The package configuration should be present (see mod-builds.cxx for
        // details) but if it is not, let's log the warning.
        //
        if (cfg != nullptr)
        {
          // Don't send the build notification email if the task result is
          // `skip`, the configuration is hidden, or is now excluded by the
          // package.
          //
          if (rs != result_status::skip && !belongs (*tc, "hidden"))
          {
            build_db_->load (*pkg, pkg->constraints_section);

            if (!exclude (*cfg, pkg->builds, pkg->constraints, *tc))
              bld = b;
          }
        }
        else
          warn << "cannot find configuration '" << b->package_config_name
               << "' for package " << pkg->id.name << '/' << pkg->version;
      }

      // If required, stash the service notification information.
      //
      if (tsb != nullptr || tsq != nullptr)
      {
        assert (t != nullptr);

        tss = make_pair (move (*t->service), move (b));
      }
    }

    t.commit ();
  }

  // We either notify about the queued build or notify about the built package
  // or don't notify at all.
  //
  assert (tsb == nullptr || tsq == nullptr);

  // If the package build is interrupted and the tenant-associated third-party
  // service needs to be notified about the queued builds, then call the
  // tenant_service_build_queued::build_queued() callback function and update
  // the service state, if requested.
  //
  if (tsq != nullptr)
  {
    assert (tss); // Wouldn't be here otherwise.

    const tenant_service& ss (tss->first);

    vector<build> qbs;
    qbs.push_back (move (*tss->second));

    // Release the database connection since build_queued() notification can
    // potentially be time-consuming (e.g., it may perform an HTTP request).
    //
    conn.reset ();

    if (auto f = tsq->build_queued (qbs.back ().tenant,
                                    ss,
                                    qbs,
                                    build_state::building,
                                    qhs,
                                    log_writer_))
    {
      conn = build_db_->connection ();
      update_tenant_service_state (conn,
                                   tenant_service_map_,
                                   ss.type, ss.id,
                                   f);
    }
  }

  // If a third-party service needs to be notified about the built package,
  // then call the tenant_service_build_built::build_built() callback function
  // and update the service state, if requested.
  //
  if (tsb != nullptr)
  {
    assert (tss); // Wouldn't be here otherwise.

    tenant_service& ss (tss->first);
    const build& b (*tss->second);

    // Release the database connection since build_built() notification can
    // potentially be time-consuming (e.g., it may perform an HTTP request).
    //
    conn.reset ();

    if (auto f = tsb->build_built (b.tenant, ss, b, log_writer_))
    {
      conn = build_db_->connection ();

      bool build_completed (false);

      if (optional<string> data =
          update_tenant_service_state (
            conn,
            tenant_service_map_,
            ss.type, ss.id,
            [&f, &build_completed] (const string& tid,
                                    const tenant_service& ts)
            {
              auto r (f (tid, ts));
              build_completed = r.second;
              return move (r.first);
            }))
      {
        ss.data = move (data);
      }

      if (build_completed)
      {
        // Release the database connection since the build_completed()
        // notification can potentially be time-consuming (e.g., it may
        // perform an HTTP request).
        //
        conn.reset ();

        tsb->build_completed (b.tenant, ss, log_writer_);
      }
    }
  }

  if (bld != nullptr)
  {
    // Don't sent the notification email for success-to-success status change,
    // etc.
    //
    if (!build_notify)
      (cfg->email ? cfg->email : pkg->build_email) = email ();

    if (conn == nullptr)
      conn = build_db_->connection ();

    send_notification_email (*options_,
                             conn,
                             *bld,
                             *pkg,
                             *cfg,
                             unforced ? "build" : "rebuild",
                             error,
                             verb_ >= 2 ? &trace : nullptr);
  }

  return true;
}
