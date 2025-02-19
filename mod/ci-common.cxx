// file      : mod/ci-common.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/ci-common.hxx>

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <libbutl/uuid.hxx>
#include <libbutl/fdstream.hxx>
#include <libbutl/sendmail.hxx>
#include <libbutl/timestamp.hxx>
#include <libbutl/filesystem.hxx>
#include <libbutl/process-io.hxx>          // operator<<(ostream, process_args)
#include <libbutl/manifest-serializer.hxx>

#include <libbrep/build.hxx>
#include <libbrep/build-odb.hxx>
#include <libbrep/build-package.hxx>
#include <libbrep/build-package-odb.hxx>

#include <mod/utility.hxx>          // sleep_before_retry()
#include <mod/database-module.hxx>  // database_module::cancel_tenant()
#include <mod/external-handler.hxx>

namespace brep
{
  using namespace std;
  using namespace butl;

  void ci_start::
  init (shared_ptr<options::ci_start> o)
  {
    // Verify the data directory satisfies the requirements.
    //
    const dir_path& d (o->ci_data ());

    if (d.relative ())
      throw runtime_error ("ci-data directory path must be absolute");

    if (!dir_exists (d))
      throw runtime_error ("ci-data directory '" + d.string () +
                           "' does not exist");

    if (o->ci_handler_specified () && o->ci_handler ().relative ())
      throw runtime_error ("ci-handler path must be absolute");

    options_ = move (o);
  }

  static optional<ci_start::start_result>
  start (const basic_mark& error,
         const basic_mark& warn,
         const basic_mark* trace,
         const options::ci_start& ops,
         string&& request_id,
         optional<tenant_service>&& service,
         bool service_load,
         const repository_location& repository,
         const vector<ci_start::package>& packages,
         const optional<string>& client_ip,
         const optional<string>& user_agent,
         const optional<string>& interactive,
         const optional<string>& simulate,
         const vector<pair<string, string>>& custom_request,
         const vector<pair<string, string>>& overrides)
  {
    using serializer    = manifest_serializer;
    using serialization = manifest_serialization;

    using result = ci_start::start_result;

    // If the tenant service is specified, then its type may not be empty.
    //
    assert (!service || !service->type.empty ());

    // Create the submission data directory.
    //
    dir_path dd (ops.ci_data () / dir_path (request_id));

    try
    {
      // It's highly unlikely but still possible that the directory already
      // exists. This can only happen if the generated uuid is not unique.
      //
      if (try_mkdir (dd) == mkdir_status::already_exists)
        throw_generic_error (EEXIST);
    }
    catch (const system_error& e)
    {
      error << "unable to create directory '" << dd << "': " << e;
      return nullopt;
    }

    auto_rmdir ddr (dd);

    // Return the start_result object for the client errors (normally the bad
    // request status code (400) for the client data serialization errors).
    //
    auto client_error = [&request_id] (uint16_t status, string message)
    {
      return result {status,
                     move (message),
                     request_id,
                     vector<pair<string, string>> ()};
    };

    // Serialize the CI request manifest to a stream. On the serialization
    // error return false together with the start_result object containing the
    // bad request (400) code and the error message. On the stream error pass
    // through the io_error exception. Otherwise return true.
    //
    timestamp ts (system_clock::now ());

    auto rqm = [&request_id,
                &ts,
                &service,
                service_load,
                &repository,
                &packages,
                &client_ip,
                &user_agent,
                &interactive,
                &simulate,
                &custom_request,
                &client_error] (ostream& os, bool long_lines = false)
      -> pair<bool, optional<result>>
    {
      try
      {
        serializer s (os, "request", long_lines);

        // Serialize the submission manifest header.
        //
        s.next ("", "1"); // Start of manifest.
        s.next ("id", request_id);
        s.next ("repository", repository.string ());

        for (const ci_start::package& p: packages)
        {
          if (!p.version)
            s.next ("package", p.name.string ());
          else
            s.next ("package",
                    p.name.string () + '/' + p.version->string ());
        }

        if (interactive)
          s.next ("interactive", *interactive);

        if (simulate)
          s.next ("simulate", *simulate);

        s.next ("timestamp",
                butl::to_string (ts,
                                 "%Y-%m-%dT%H:%M:%SZ",
                                 false /* special */,
                                 false /* local */));

        if (client_ip)
          s.next ("client-ip", *client_ip);

        if (user_agent)
          s.next ("user-agent", *user_agent);

        if (service)
        {
          // Note that if the service id is not specified, then the handler
          // will use the generated reference instead.
          //
          if (!service->id.empty ())
            s.next ("service-id", service->id);

          s.next ("service-type", service->type);

          if (service->data)
            s.next ("service-data", *service->data);

          s.next ("service-action", service_load ? "load" : "start");
        }

        // Serialize the request custom parameters.
        //
        // Note that the serializer constraints the custom parameter names
        // (can't start with '#', can't contain ':' and the whitespaces,
        // etc).
        //
        for (const pair<string, string>& nv: custom_request)
          s.next (nv.first, nv.second);

        s.next ("", ""); // End of manifest.
        return make_pair (true, optional<result> ());
      }
      catch (const serialization& e)
      {
        return make_pair (false,
                          optional<result> (
                            client_error (400,
                                          string ("invalid parameter: ") +
                                          e.what ())));
      }
    };

    // Serialize the CI request manifest to the submission directory.
    //
    path rqf (dd / "request.manifest");

    try
    {
      ofdstream os (rqf);
      pair<bool, optional<result>> r (rqm (os));
      os.close ();

      if (!r.first)
        return move (*r.second);
    }
    catch (const io_error& e)
    {
      error << "unable to write to '" << rqf << "': " << e;
      return nullopt;
    }

    // Serialize the CI overrides manifest to a stream. On the serialization
    // error return false together with the start_result object containing the
    // bad request (400) code and the error message. On the stream error pass
    // through the io_error exception. Otherwise return true.
    //
    auto ovm = [&overrides, &client_error] (ostream& os,
                                            bool long_lines = false)
      -> pair<bool, optional<result>>
    {
      try
      {
        serializer s (os, "overrides", long_lines);

        s.next ("", "1"); // Start of manifest.

        for (const pair<string, string>& nv: overrides)
          s.next (nv.first, nv.second);

        s.next ("", ""); // End of manifest.
        return make_pair (true, optional<result> ());
      }
      catch (const serialization& e)
      {
        return make_pair (false,
                          optional<result> (
                            client_error (
                              400,
                              string ("invalid manifest override: ") +
                              e.what ())));
      }
    };

    // Serialize the CI overrides manifest to the submission directory.
    //
    path ovf (dd / "overrides.manifest");

    if (!overrides.empty ())
    try
    {
      ofdstream os (ovf);
      pair<bool, optional<result>> r (ovm (os));
      os.close ();

      if (!r.first)
        return move (*r.second);
    }
    catch (const io_error& e)
    {
      error << "unable to write to '" << ovf << "': " << e;
      return nullopt;
    }

    // Given that the submission data is now successfully persisted we are no
    // longer in charge of removing it, except for the cases when the
    // submission handler terminates with an error (see below for details).
    //
    ddr.cancel ();

    // If the handler terminates with non-zero exit status or specifies 5XX
    // (HTTP server error) submission result manifest status value, then we
    // stash the submission data directory for troubleshooting. Otherwise, if
    // it's the 4XX (HTTP client error) status value, then we remove the
    // directory.
    //
    auto stash_submit_dir = [&dd, error] ()
    {
      if (dir_exists (dd))
      try
      {
        mvdir (dd, dir_path (dd + ".fail"));
      }
      catch (const system_error& e)
      {
        // Not much we can do here. Let's just log the issue and bail out
        // leaving the directory in place.
        //
        error << "unable to rename directory '" << dd << "': " << e;
      }
    };

    // Run the submission handler, if specified, reading the CI result
    // manifest from its stdout and parse it into the resulting manifest
    // object. Otherwise, create implied CI result manifest.
    //
    result sr;

    if (ops.ci_handler_specified ())
    {
      using namespace external_handler;

      optional<result_manifest> r (run (ops.ci_handler (),
                                        ops.ci_handler_argument (),
                                        dd,
                                        ops.ci_handler_timeout (),
                                        error,
                                        warn,
                                        trace));
      if (!r)
      {
        stash_submit_dir ();
        return nullopt; // The diagnostics is already issued.
      }

      sr.status = r->status;

      for (manifest_name_value& nv: r->values)
      {
        string& n (nv.name);
        string& v (nv.value);

        if (n == "message")
          sr.message = move (v);
        else if (n == "reference")
          sr.reference = move (v);
        else if (n != "status")
          sr.custom_result.emplace_back (move (n), move (v));
      }

      if (sr.reference.empty ())
        sr.reference = move (request_id);
    }
    else // Create the implied CI result manifest.
    {
      sr.status = 200;
      sr.message = "CI request is queued";
      sr.reference = move (request_id);
    }

    // Serialize the CI result manifest manifest to a stream. On the
    // serialization error log the error description and return false, on the
    // stream error pass through the io_error exception, otherwise return
    // true.
    //
    auto rsm = [&sr, &error] (ostream& os, bool long_lines = false) -> bool
    {
      try
      {
        ci_start::serialize_manifest (sr, os, long_lines);
        return true;
      }
      catch (const serialization& e)
      {
        error << "ref " << sr.reference << ": unable to serialize handler's "
              << "output: " << e;
        return false;
      }
    };

    // If the submission data directory still exists then perform an
    // appropriate action on it, depending on the submission result status.
    // Note that the handler could move or remove the directory.
    //
    if (dir_exists (dd))
    {
      // Remove the directory if the client error is detected.
      //
      if (sr.status >= 400 && sr.status < 500)
      {
        rmdir_r (dd);
      }
      //
      // Otherwise, save the result manifest, into the directory. Also stash
      // the directory for troubleshooting in case of the server error.
      //
      else
      {
        path rsf (dd / "result.manifest");

        try
        {
          ofdstream os (rsf);

          // Not being able to stash the result manifest is not a reason to
          // claim the submission failed. The error is logged nevertheless.
          //
          rsm (os);

          os.close ();
        }
        catch (const io_error& e)
        {
          // Not fatal (see above).
          //
          error << "unable to write to '" << rsf << "': " << e;
        }

        if (sr.status >= 500 && sr.status < 600)
          stash_submit_dir ();
      }
    }

    // Send email, if configured, and the CI request submission is not
    // simulated. Use the long lines manifest serialization mode for the
    // convenience of copying/clicking URLs they contain.
    //
    // Note that we don't consider the email sending failure to be a
    // submission failure as the submission data is successfully persisted and
    // the handler is successfully executed, if configured. One can argue that
    // email can be essential for the submission processing and missing it
    // would result in the incomplete submission. In this case it's natural to
    // assume that the web server error log is monitored and the email sending
    // failure will be noticed.
    //
    if (ops.ci_email_specified () && !simulate)
    try
    {
      // Redirect the diagnostics to the web server error log.
      //
      sendmail sm ([trace] (const char* args[], size_t n)
                   {
                     if (trace != nullptr)
                       *trace << process_args {args, n};
                   },
                   2 /* stderr */,
                   ops.email (),
                   ((service ? service->type : "ci") +
                    " request submission: " + repository.string ()),
                   {ops.ci_email ()});

      // Write the CI request manifest.
      //
      pair<bool, optional<result>> r (rqm (sm.out, true /* long_lines */));

      assert (r.first); // The serialization succeeded once, so can't fail now.

      // Write the CI overrides manifest.
      //
      sm.out << "\n\n";

      r = ovm (sm.out, true /* long_lines */);
      assert (r.first); // The serialization succeeded once, so can't fail now.

      // Write the CI result manifest.
      //
      sm.out << "\n\n";

      // We don't care about the result (see above).
      //
      rsm (sm.out, true /* long_lines */);

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

    return optional<result> (move (sr));
  }

  optional<ci_start::start_result> ci_start::
  start (const basic_mark& error,
         const basic_mark& warn,
         const basic_mark* trace,
         optional<tenant_service>&& service,
         const repository_location& repository,
         const vector<package>& packages,
         const optional<string>& client_ip,
         const optional<string>& user_agent,
         const optional<string>& interactive,
         const optional<string>& simulate,
         const vector<pair<string, string>>& custom_request,
         const vector<pair<string, string>>& overrides) const
  {
    assert (options_ != nullptr); // Shouldn't be called otherwise.

    // Generate the request id.
    //
    // Note that it will also be used as a CI result manifest reference,
    // unless the latter is provided by the external handler.
    //
    string request_id;

    try
    {
      request_id = uuid::generate ().string ();
    }
    catch (const system_error& e)
    {
      error << "unable to generate request id: " << e;
      return nullopt;
    }

    return brep::start (error, warn, trace,
                        *options_,
                        move (request_id),
                        move (service),
                        false /* service_load */,
                        repository,
                        packages,
                        client_ip,
                        user_agent,
                        interactive,
                        simulate,
                        custom_request,
                        overrides);
  }

  void ci_start::
  serialize_manifest (const start_result& r, ostream& os, bool long_lines)
  {
    manifest_serializer s (os, "result", long_lines);

    s.next ("", "1");                        // Start of manifest.
    s.next ("status", to_string (r.status));
    s.next ("message", r.message);
    s.next ("reference", r.reference);

    for (const pair<string, string>& nv: r.custom_result)
      s.next (nv.first, nv.second);

    s.next ("", "");                         // End of manifest.
  }

  optional<pair<string, ci_start::duplicate_tenant_result>> ci_start::
  create (const basic_mark& error,
          const basic_mark&,
          const basic_mark* trace,
          odb::core::database& db,
          size_t retry_max,
          tenant_service&& service,
          duration notify_interval,
          duration notify_delay,
          duplicate_tenant_mode mode) const
  {
    using namespace odb::core;

    assert (mode == duplicate_tenant_mode::fail || !service.id.empty ());
    assert (!transaction::has_current ());

    build_tenant t;

    // Set the reference count to 1 for the `created` result.
    //
    duplicate_tenant_result r (duplicate_tenant_result::created);
    service.ref_count = 1;

    string request_id;
    for (size_t retry (0);;)
    {
      try
      {
        transaction tr (db.begin ());

        // Unless we are in the 'fail on duplicate' mode, check if this
        // service type/id pair is already in use and, if that's the case,
        // either ignore it or reassign this service to a new tenant,
        // canceling the old one.
        //
        if (mode != duplicate_tenant_mode::fail)
        {
          using query = query<build_tenant>;

          shared_ptr<build_tenant> t (
            db.query_one<build_tenant> (query::service.id == service.id &&
                                        query::service.type == service.type));
          if (t != nullptr)
          {
            // Reduce the replace_archived mode to the replace or ignore mode.
            //
            if (mode == duplicate_tenant_mode::replace_archived)
            {
              mode = (t->archived
                      ? duplicate_tenant_mode::replace
                      : duplicate_tenant_mode::ignore);
            }

            // Shouldn't be here otherwise.
            //
            assert (t->service);

            // Bail out in the ignore mode and cancel the tenant in the
            // replace mode.
            //
            if (mode == duplicate_tenant_mode::ignore)
            {
              // Increment the reference count for the `ignored` result.
              //
              ++(t->service->ref_count);

              db.update (t);
              tr.commit ();

              return make_pair (move (t->id), duplicate_tenant_result::ignored);
            }

            assert (mode == duplicate_tenant_mode::replace);

            // Preserve the current reference count for the `replaced` result.
            //
            service.ref_count = t->service->ref_count;

            if (t->unloaded_timestamp)
            {
              db.erase (t);
            }
            else
            {
              t->service = nullopt;
              t->archived = true;
              db.update (t);
            }

            r = duplicate_tenant_result::replaced;
          }
        }

        // Generate the request id.
        //
        if (request_id.empty ())
        try
        {
          request_id = uuid::generate ().string ();
        }
        catch (const system_error& e)
        {
          error << "unable to generate request id: " << e;
          return nullopt;
        }

        // Use the generated request id if the tenant service id is not
        // specified.
        //
        if (service.id.empty ())
          service.id = request_id;

        t = build_tenant (move (request_id),
                          move (service),
                          system_clock::now () - notify_interval + notify_delay,
                          notify_interval);

        // Note that in contrast to brep-load, we know that the tenant id is
        // unique and thus we don't try to remove a tenant with such an id.
        // There is also not much reason to assume that we may have switched
        // from the single-tenant mode here and remove the respective tenant,
        // unless we are in the tenant-service functionality development mode.
        //
#ifdef BREP_CI_TENANT_SERVICE_UNLOADED
        cstrings ts ({""});

        db.erase_query<build_package> (
          query<build_package>::id.tenant.in_range (ts.begin (), ts.end ()));

        db.erase_query<build_repository> (
          query<build_repository>::id.tenant.in_range (ts.begin (), ts.end ()));

        db.erase_query<build_public_key> (
          query<build_public_key>::id.tenant.in_range (ts.begin (), ts.end ()));

        db.erase_query<build_tenant> (
          query<build_tenant>::id.in_range (ts.begin (), ts.end ()));
#endif

        db.persist (t);

        tr.commit ();

        if (trace != nullptr)
          *trace << "unloaded CI request " << t.id << " for service "
                 << t.service->id << ' ' << t.service->type << " is created";

        // Bail out if we have successfully erased, updated, or persisted the
        // tenant object.
        //
        break;
      }
      catch (const odb::recoverable& e)
      {
        // If no more retries left, don't re-throw odb::recoverable not to
        // retry at the upper level.
        //
        if (retry == retry_max)
          throw runtime_error (e.what ());

        // Prepare for the next iteration.
        //
        request_id = move (t.id);
        service = move (*t.service);
        service.ref_count = 1;
        r = duplicate_tenant_result::created;

        sleep_before_retry (retry++);
      }
    }

    return make_pair (move (t.id), r);
  }

  optional<ci_start::start_result> ci_start::
  load (const basic_mark& error,
        const basic_mark& warn,
        const basic_mark* trace,
        odb::core::database& db,
        size_t retry_max,
        tenant_service&& service,
        const repository_location& repository) const
  {
    using namespace odb::core;

    string request_id;

    for (size_t retry (0);;)
    {
      try
      {
        assert (!transaction::has_current ());

        transaction tr (db.begin ());

        using query = query<build_tenant>;

        shared_ptr<build_tenant> t (
          db.query_one<build_tenant> (query::service.id == service.id &&
                                      query::service.type == service.type));

        if (t == nullptr)
        {
          error << "unable to find tenant for service " << service.id << ' '
                << service.type;

          return nullopt;
        }
        else if (t->archived)
        {
          error << "tenant " << t->id << " for service " << service.id << ' '
                << service.type << " is already archived";

          return nullopt;
        }
        else if (!t->unloaded_timestamp)
        {
          error << "tenant " << t->id << " for service " << service.id << ' '
                << service.type << " is already loaded";

          return nullopt;
        }

        t->unloaded_timestamp = nullopt;
        db.update (t);

        tr.commit ();

        request_id = move (t->id);

        // Bail out if we have successfully updated the tenant object.
        //
        break;
      }
      catch (const odb::recoverable& e)
      {
        // If no more retries left, don't re-throw odb::recoverable not to
        // retry at the upper level.
        //
        if (retry == retry_max)
          throw runtime_error (e.what ());

        sleep_before_retry (retry++);
      }
    }

    assert (options_ != nullptr); // Shouldn't be called otherwise.

    optional<start_result> r (brep::start (error, warn, trace,
                                           *options_,
                                           move (request_id),
                                           move (service),
                                           true /* service_load */,
                                           repository,
                                           {} /* packages */,
                                           nullopt /* client_ip */,
                                           nullopt /* user_agent */,
                                           nullopt /* interactive */,
                                           nullopt /* simulate */,
                                           {} /* custom_request */,
                                           {} /* overrides */));

    // Note: on error (r == nullopt) the diagnostics is already issued.
    //
    if (trace != nullptr && r)
      *trace << "CI request for '" << repository << "' is "
             << (r->status != 200 ? "not " : "") << "loaded: "
             << r->message << " (reference: " << r->reference << ')';

    return r;
  }

  optional<tenant_service> ci_start::
  cancel (const basic_mark&,
          const basic_mark&,
          const basic_mark* trace,
          odb::core::database& db,
          size_t retry_max,
          const string& type,
          const string& id,
          bool ref_count) const
  {
    using namespace odb::core;

    assert (!transaction::has_current ());

    optional<tenant_service> r;

    for (size_t retry (0);;)
    {
      try
      {
        transaction tr (db.begin ());

        using query = query<build_tenant>;

        shared_ptr<build_tenant> t (
          db.query_one<build_tenant> (query::service.id == id &&
                                      query::service.type == type));
        if (t == nullptr)
          return nullopt;

        // Shouldn't be here otherwise.
        //
        assert (t->service && t->service->ref_count != 0);

        bool cancel (!ref_count || --(t->service->ref_count) == 0);

        if (cancel)
        {
          // Move out the service state before it is dropped from the tenant.
          //
          r = move (t->service);

          if (t->unloaded_timestamp)
          {
            db.erase (t);
          }
          else if (!t->archived || t->service)
          {
            t->service = nullopt;
            t->archived = true;
            db.update (t);
          }

          if (trace != nullptr)
            *trace << "CI request " << t->id << " for service " << id << ' '
                   << type << " is canceled";
        }
        else
        {
          db.update (t); // Update the service reference count.

          // Move out the service state after the tenant is updated.
          //
          r = move (t->service);
        }

        tr.commit ();

        // Bail out if we have successfully updated or erased the tenant
        // object.
        //
        break;
      }
      catch (const odb::recoverable& e)
      {
        // If no more retries left, don't re-throw odb::recoverable not to
        // retry at the upper level.
        //
        if (retry == retry_max)
          throw runtime_error (e.what ());

        r = nullopt; // Prepare for the next iteration.

        // Try to cancel as fast as possible.
        //
#if 0
        sleep_before_retry (retry++);
#else
        retry++;
#endif
      }
    }

    return r;
  }

  bool ci_start::
  cancel (const basic_mark&,
          const basic_mark&,
          const basic_mark* trace,
          const string& reason,
          odb::core::database& db,
          size_t retry_max,
          const string& tid) const
  {
    using namespace odb::core;

    assert (!transaction::has_current ());

    for (size_t retry (0);;)
    {
      try
      {
        transaction tr (db.begin ());

        shared_ptr<build_tenant> t (db.find<build_tenant> (tid));

        if (t == nullptr)
          return false;

        if (t->unloaded_timestamp)
        {
          db.erase (t);
        }
        else if (!t->archived)
        {
          t->archived = true;
          db.update (t);
        }

        tr.commit ();

        // Bail out if we have successfully updated or erased the tenant
        // object.
        //
        break;
      }
      catch (const odb::recoverable& e)
      {
        // If no more retries left, don't re-throw odb::recoverable not to
        // retry at the upper level.
        //
        if (retry == retry_max)
          throw runtime_error (e.what ());

        // Try to cancel as fast as possible.
        //
#if 0
        sleep_before_retry (retry++);
#else
        retry++;
#endif
      }
    }

    if (trace != nullptr)
      *trace << "CI request " << tid << " is canceled: "
             << (reason.size () < 50
                 ? reason
                 : string (reason, 0, 50) + "...");

    return true;
  }

  optional<build_state> ci_start::
  rebuild (odb::core::database& db,
           size_t retry_max,
           const tenant_service_map& tsm,
           const diag_epilogue& log_writer,
           const build_id& id,
           function<optional<string> (const string& tenant_id,
                                      const tenant_service&,
                                      build_state)> uf) const
  {
    using namespace odb::core;

    build_state s;
    shared_ptr<build_tenant> unsaved_data;

    // Use the database connection for starting the transaction. This way, if
    // no more retries after the recoverable database failures left, we can
    // reuse the connection for the cancel_tenant() call to cancel as fast as
    // possible, not wasting time on re-acquiring it.
    //
    connection_ptr conn (db.connection ());

    for (size_t retry (0);;)
    {
      try
      {
        // NOTE: don't forget to update build_force::handle() if changing
        //       anything here.
        //
        transaction t (conn->begin ());

        package_build pb;
        if (!db.query_one<package_build> (query<package_build>::build::id == id,
                                          pb) ||
            pb.archived)
        {
          return nullopt;
        }

        const shared_ptr<build>& b (pb.build);
        s = b->state;

        if (s != build_state::queued)
        {
          force_state force (s == build_state::built
                             ? force_state::forced
                             : force_state::forcing);

          if (b->force != force)
          {
            b->force = force;
            db.update (b);
          }

          if (uf != nullptr)
          {
            shared_ptr<build_tenant> t (db.load<build_tenant> (b->tenant));

            assert (t->service);

            tenant_service& ts (*t->service);

            if (optional<string> data = uf (t->id, ts, s))
            {
              ts.data = move (*data);

              // If this is our last chance to persist the service data
              // change, then stash the tenant for cancellation on a potential
              // failure to persist.
              //
              if (retry == retry_max)
                unsaved_data = t;

              db.update (t);
            }
          }
        }

        t.commit ();

        // Bail out if we have successfully updated the build and tenant
        // objects.
        //
        break;
      }
      catch (const odb::recoverable& e)
      {
        // If no more retries left, don't re-throw odb::recoverable not to
        // retry at the upper level.
        //
        if (retry == retry_max)
        {
          // Cancel the tenant if we failed to persist the service data
          // change.
          //
          if (unsaved_data != nullptr)
          {
            NOTIFICATION_DIAG (log_writer);

            const string& tid (unsaved_data->id);
            const tenant_service& ts (*unsaved_data->service);

            error << e << "; no tenant service state update retries left, "
                  << "canceling tenant " << tid << " for service " << ts.id
                  << ' ' << ts.type;

            try
            {
              database_module::cancel_tenant (move (conn), retry_max,
                                              tsm, log_writer,
                                              tid, ts);
            }
            catch (const runtime_error& e)
            {
              error << e << "; no retries left to cancel tenant " << tid
                    << " for service " << ts.id << ' ' << ts.type;

              // Fall through to throw.
            }
          }

          throw runtime_error (e.what ());
        }

        // Release the database connection before the sleep and re-acquire it
        // afterwards.
        //
        conn.reset ();
        sleep_before_retry (retry++);
        conn = db.connection ();
      }
    }

    return s;
  }

  optional<ci_start::tenant_data> ci_start::
  find (odb::core::database& db,
        const string& type,
        const string& id) const
  {
    using namespace odb::core;

    assert (!transaction::has_current ());

    transaction tr (db.begin ());

    using query = query<build_tenant>;

    shared_ptr<build_tenant> t (
      db.query_one<build_tenant> (query::service.id == id &&
                                  query::service.type == type));

    tr.commit ();

    if (t == nullptr || !t->service)
      return nullopt;

    return tenant_data {move (t->id), move (*t->service), t->archived};
  }
}
