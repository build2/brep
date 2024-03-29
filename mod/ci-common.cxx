// file      : mod/ci-common.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/ci-common.hxx>

#include <libbutl/uuid.hxx>
#include <libbutl/fdstream.hxx>
#include <libbutl/sendmail.hxx>
#include <libbutl/timestamp.hxx>
#include <libbutl/filesystem.hxx>
#include <libbutl/process-io.hxx>          // operator<<(ostream, process_args)
#include <libbutl/manifest-serializer.hxx>

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
         const vector<pair<string, string>>& overrides)
  {
    using serializer    = manifest_serializer;
    using serialization = manifest_serialization;

    assert (options_ != nullptr); // Shouldn't be called otherwise.

    // If the tenant service is specified, then its type may not be empty.
    //
    assert (!service || !service->type.empty ());

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

    // Create the submission data directory.
    //
    dir_path dd (options_->ci_data () / dir_path (request_id));

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
      return start_result {status,
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
                &repository,
                &packages,
                &client_ip,
                &user_agent,
                &interactive,
                &simulate,
                &custom_request,
                &client_error] (ostream& os, bool long_lines = false)
      -> pair<bool, optional<start_result>>
    {
      try
      {
        serializer s (os, "request", long_lines);

        // Serialize the submission manifest header.
        //
        s.next ("", "1"); // Start of manifest.
        s.next ("id", request_id);
        s.next ("repository", repository.string ());

        for (const package& p: packages)
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
        return make_pair (true, optional<start_result> ());
      }
      catch (const serialization& e)
      {
        return make_pair (false,
                          optional<start_result> (
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
      pair<bool, optional<start_result>> r (rqm (os));
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
      -> pair<bool, optional<start_result>>
    {
      try
      {
        serializer s (os, "overrides", long_lines);

        s.next ("", "1"); // Start of manifest.

        for (const pair<string, string>& nv: overrides)
          s.next (nv.first, nv.second);

        s.next ("", ""); // End of manifest.
        return make_pair (true, optional<start_result> ());
      }
      catch (const serialization& e)
      {
        return make_pair (false,
                          optional<start_result> (
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
      pair<bool, optional<start_result>> r (ovm (os));
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
    start_result sr;

    if (options_->ci_handler_specified ())
    {
      using namespace external_handler;

      optional<result_manifest> r (run (options_->ci_handler (),
                                        options_->ci_handler_argument (),
                                        dd,
                                        options_->ci_handler_timeout (),
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
        serialize_manifest (sr, os, long_lines);
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
    if (options_->ci_email_specified () && !simulate)
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
                   options_->email (),
                   "CI request submission (" + sr.reference + ')',
                   {options_->ci_email ()});

      // Write the CI request manifest.
      //
      pair<bool, optional<start_result>> r (
        rqm (sm.out, true /* long_lines */));

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

    return optional<start_result> (move (sr));
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
}
