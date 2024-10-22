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

#include <libbrep/build-package.hxx>
#include <libbrep/build-package-odb.hxx>

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
                   "CI request submission (" + sr.reference + ')',
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

  pair<optional<string>, ci_start::duplicate_tenant_result> ci_start::
  create (const basic_mark& error,
          const basic_mark&,
          const basic_mark* trace,
          odb::core::database& db,
          tenant_service&& service,
          duration notify_interval,
          duration notify_delay,
          duplicate_tenant_mode) const
  {
    using namespace odb::core;

    // Generate the request id.
    //
    string request_id;

    try
    {
      request_id = uuid::generate ().string ();
    }
    catch (const system_error& e)
    {
      error << "unable to generate request id: " << e;
      return {nullopt, duplicate_tenant_result::ignored}; // @@ TODO HACKED AROUND
    }

    // Use the generated request id if the tenant service id is not specified.
    //
    if (service.id.empty ())
      service.id = request_id;

    build_tenant t (move (request_id),
                    move (service),
                    system_clock::now () - notify_interval + notify_delay,
                    notify_interval);
    {
      assert (!transaction::has_current ());

      transaction tr (db.begin ());

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
    }

    if (trace != nullptr)
      *trace << "unloaded CI request " << t.id << " for service "
             << t.service->id << ' ' << t.service->type << " is created";

    return {move (t.id), duplicate_tenant_result::created}; // @@ TODO HACKED AROUND
  }

  optional<ci_start::start_result> ci_start::
  load (const basic_mark& error,
        const basic_mark& warn,
        const basic_mark* trace,
        odb::core::database& db,
        tenant_service&& service,
        const repository_location& repository) const
  {
    using namespace odb::core;

    string request_id;
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
    if (t == nullptr)
      return nullopt;

    // @@ Why not remove it if unloaded (and below)?

    optional<tenant_service> r (move (t->service));
    t->service = nullopt;
    t->archived = true;
    db.update (t);

    tr.commit ();

    if (trace != nullptr)
      *trace << "CI request " << t->id << " for service " << id << ' ' << type
             << " is canceled";

    return r;
  }

  bool ci_start::
  cancel (const basic_mark&,
          const basic_mark&,
          const basic_mark* trace,
          const string& reason,
          odb::core::database& db,
          const string& tid) const
  {
    using namespace odb::core;

    assert (!transaction::has_current ());

    transaction tr (db.begin ());

    shared_ptr<build_tenant> t (db.find<build_tenant> (tid));

    if (t == nullptr)
      return false;

    if (!t->archived)
    {
      t->archived = true;
      db.update (t);
    }

    tr.commit ();

    if (trace != nullptr)
      *trace << "CI request " << tid << " is canceled: "
             << (reason.size () < 50
                 ? reason
                 : string (reason, 0, 50) + "...");

    return true;
  }
}
