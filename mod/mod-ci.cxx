// file      : mod/mod-ci.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-ci.hxx>

#include <ostream>

#include <libbutl/uuid.hxx>
#include <libbutl/sendmail.mxx>
#include <libbutl/fdstream.mxx>
#include <libbutl/timestamp.mxx>
#include <libbutl/filesystem.mxx>
#include <libbutl/process-io.mxx>          // operator<<(ostream, process_args)
#include <libbutl/manifest-parser.mxx>
#include <libbutl/manifest-serializer.mxx>

#include <libbpkg/manifest.hxx>
#include <libbpkg/package-name.hxx>

#include <web/xhtml.hxx>
#include <web/module.hxx>

#include <mod/page.hxx>
#include <mod/options.hxx>
#include <mod/external-handler.hxx>

using namespace std;
using namespace butl;
using namespace web;
using namespace brep::cli;

brep::ci::
ci (const ci& r)
    : handler (r),
      options_ (r.initialized_ ? r.options_ : nullptr),
      form_ (r.initialized_ || r.form_ == nullptr
             ? r.form_
             : make_shared<xhtml::fragment> (*r.form_))
{
}

void brep::ci::
init (scanner& s)
{
  HANDLER_DIAG;

  options_ = make_shared<options::ci> (
    s, unknown_mode::fail, unknown_mode::fail);

  // Verify that the CI request handling is setup properly, if configured.
  //
  if (options_->ci_data_specified ())
  {
    // Verify the data directory satisfies the requirements.
    //
    const dir_path& d (options_->ci_data ());

    if (d.relative ())
      fail << "ci-data directory path must be absolute";

    if (!dir_exists (d))
      fail << "ci-data directory '" << d << "' does not exist";

    // Parse XHTML5 form file, if configured.
    //
    if (options_->ci_form_specified ())
    {
      const path& ci_form (options_->ci_form ());

      if (ci_form.relative ())
        fail << "ci-form path must be absolute";

      try
      {
        ifdstream is (ci_form);

        form_ = make_shared<xhtml::fragment> (is.read_text (),
                                              ci_form.string ());
      }
      catch (const xml::parsing& e)
      {
        fail << "unable to parse ci-form file: " << e;
      }
      catch (const io_error& e)
      {
        fail << "unable to read ci-form file '" << ci_form << "': " << e;
      }
    }

    if (options_->ci_handler_specified () &&
        options_->ci_handler ().relative ())
      fail << "ci-handler path must be absolute";
  }

  if (options_->root ().empty ())
    options_->root (dir_path ("/"));
}

bool brep::ci::
handle (request& rq, response& rs)
{
  using namespace bpkg;
  using namespace xhtml;

  using serializer    = manifest_serializer;
  using serialization = manifest_serialization;

  HANDLER_DIAG;

  const dir_path& root (options_->root ());

  // We will respond with the manifest to the CI request submission protocol
  // violations and with a plain text message on the internal errors. In the
  // latter case we will always respond with the same neutral message for
  // security reason, logging the error details. Note that descriptions of
  // exceptions caught by the web server are returned to the client (see
  // web/module.hxx for details), and we want to avoid this when there is a
  // danger of exposing sensitive data.
  //
  // Also we will pass through exceptions thrown by the underlying API, unless
  // we need to handle them or add details for the description, in which case
  // we will fallback to one of the above mentioned response methods.
  //
  // Note that both respond_manifest() and respond_error() are normally called
  // right before the end of the request handling. They both always return
  // true to allow bailing out with a single line, for example:
  //
  // return respond_error (); // Request is handled with an error.
  //
  string request_id; // Will be set later.
  auto respond_manifest = [&rs, &request_id] (status_code status,
                                              const string& message) -> bool
  {
    serializer s (rs.content (status, "text/manifest;charset=utf-8"),
                  "response");

    s.next ("", "1");                      // Start of manifest.
    s.next ("status", to_string (status));
    s.next ("message", message);

    if (!request_id.empty ())
      s.next ("reference", request_id);

    s.next ("", "");                       // End of manifest.
    return true;
  };

  auto respond_error = [&rs] (status_code status = 500) -> bool
  {
    rs.content (status, "text/plain;charset=utf-8")
      << "CI request submission handling failed" << endl;

    return true;
  };

  // Check if the CI request functionality is enabled.
  //
  // Note that this is not a submission protocol violation but it feels right
  // to respond with the manifest, to help the client a bit.
  //
  if (!options_->ci_data_specified ())
    return respond_manifest (404, "CI request submission disabled");

  // Parse the request form data.
  //
  const name_values& rps (rq.parameters (64 * 1024));

  // If there is no request parameters then we respond with the CI form XHTML,
  // if configured. Otherwise, will proceed as for the CI request and will fail
  // (missing parameters).
  //
  if (rps.empty () && form_ != nullptr)
  {
    const string title ("CI");

    xml::serializer s (rs.content (), title);

    s << HTML
      <<   HEAD
      <<     TITLE << title << ~TITLE
      <<     CSS_LINKS (path ("ci.css"), root)
      <<   ~HEAD
      <<   BODY
      <<     DIV_HEADER (options_->logo (), options_->menu (), root, tenant)
      <<     DIV(ID="content") << *form_ << ~DIV
      <<   ~BODY
      << ~HTML;

    return true;
  }

  // Verify the CI request parameters we expect. The unknown ones will be
  // serialized to the CI request manifest.
  //
  params::ci params;

  try
  {
    name_value_scanner s (rps);
    params = params::ci (s, unknown_mode::skip, unknown_mode::skip);
  }
  catch (const cli::exception&)
  {
    return respond_manifest (400, "invalid parameter");
  }

  const string& simulate (params.simulate ());

  if (simulate == "internal-error-text")
    return respond_error ();
  else if (simulate == "internal-error-html")
  {
    const string title ("Internal Error");
    xml::serializer s (rs.content (500), title);

    s << HTML
      <<   HEAD << TITLE << title << ~TITLE << ~HEAD
      <<   BODY << "CI request submission handling failed" << ~BODY
      << ~HTML;

    return true;
  }

  // Parse and verify the remote repository location.
  //
  repository_location rl;

  try
  {
    const repository_url& u (params.repository ());

    if (u.empty () || u.scheme == repository_protocol::file)
      throw invalid_argument ("");

    rl = repository_location (u, guess_type (u, false /* local */));
  }
  catch (const invalid_argument&)
  {
    return respond_manifest (400, "invalid repository location");
  }

  // Verify the package name[/version] arguments.
  //
  for (const string& s: params.package())
  {
    //  Let's skip the potentially unfilled package form fields.
    //
    if (s.empty ())
      continue;

    try
    {
      size_t p (s.find ('/'));

      if (p != string::npos)
      {
        package_name (string (s, 0, p));

        // Not to confuse with module::version.
        //
        bpkg::version (string (s, p + 1));
      }
      else
        package_name p (s); // Not to confuse with the s variable declaration.
    }
    catch (const invalid_argument&)
    {
      return respond_manifest (400, "invalid package " + s);
    }
  }

  // Verify that unknown parameter values satisfy the requirements (contain
  // only ASCII printable characters plus '\r', '\n', and '\t').
  //
  // Actually, the expected ones must satisfy too, so check them as well.
  //
  auto printable = [] (const string& s) -> bool
  {
    for (char c: s)
    {
      if (!((c >= 0x20 && c <= 0x7E) || c == '\n' || c == '\r' || c == '\t'))
        return false;
    }
    return true;
  };

  for (const name_value& nv: rps)
  {
    if (nv.value && !printable (*nv.value))
      return respond_manifest (400, "invalid parameter " + nv.name);
  }

  try
  {
    // Note that from now on the result manifest we respond with will contain
    // the reference value.
    //
    request_id = uuid::generate ().string ();
  }
  catch (const system_error& e)
  {
    error << "unable to generate request id: " << e;
    return respond_error ();
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
    return respond_error ();
  }

  auto_rmdir ddr (dd);

  // Serialize the CI request manifest to a stream. On the serialization error
  // respond to the client with the manifest containing the bad request (400)
  // code and return false, on the stream error pass through the io_error
  // exception, otherwise return true.
  //
  timestamp ts (system_clock::now ());

  auto rqm = [&request_id,
              &rl,
              &ts,
              &simulate,
              &rq,
              &rps,
              &params,
              &respond_manifest]
             (ostream& os) -> bool
  {
    try
    {
      serializer s (os, "request");

      // Serialize the submission manifest header.
      //
      s.next ("", "1");                // Start of manifest.
      s.next ("id", request_id);
      s.next ("repository", rl.string ());

      for (const string& p: params.package())
      {
        if (!p.empty ()) // Skip empty package names (see above for details).
          s.next ("package", p);
      }

      s.next ("timestamp",
              butl::to_string (ts,
                               "%Y-%m-%dT%H:%M:%SZ",
                               false /* special */,
                               false /* local */));

      if (!simulate.empty ())
        s.next ("simulate", simulate);

      // Serialize the User-Agent HTTP header and the client IP address.
      //
      optional<string> ip;
      optional<string> ua;
      for (const name_value& h: rq.headers ())
      {
        if (casecmp (h.name, ":Client-IP") == 0)
          ip = h.value;
        else if (casecmp (h.name, "User-Agent") == 0)
          ua = h.value;
      }

      if (ip)
        s.next ("client-ip", *ip);

      if (ua)
        s.next ("user-agent", *ua);

      // Serialize the request parameters.
      //
      // Note that the serializer constraints the parameter names (can't start
      // with '#', can't contain ':' and the whitespaces, etc.).
      //
      for (const name_value& nv: rps)
      {
        const string& n (nv.name);

        if (n != "repository" &&
            n != "_"          &&
            n != "package"    &&
            n != "simulate")
          s.next (n, nv.value ? *nv.value : "");
      }

      s.next ("", ""); // End of manifest.
      return true;
    }
    catch (const serialization& e)
    {
      respond_manifest (400, string ("invalid parameter: ") + e.what ());
      return false;
    }
  };

  // Serialize the CI request manifest to the submission directory.
  //
  path rqf (dd / "request.manifest");

  try
  {
    ofdstream os (rqf);
    bool r (rqm (os));
    os.close ();

    if (!r)
      return true; // The client is already responded with the manifest.
  }
  catch (const io_error& e)
  {
    error << "unable to write to '" << rqf << "': " << e;
    return respond_error ();
  }

  // Given that the submission data is now successfully persisted we are no
  // longer in charge of removing it, except for the cases when the submission
  // handler terminates with an error (see below for details).
  //
  ddr.cancel ();

  // If the handler terminates with non-zero exit status or specifies 5XX
  // (HTTP server error) submission result manifest status value, then we
  // stash the submission data directory for troubleshooting. Otherwise, if
  // it's the 4XX (HTTP client error) status value, then we remove the
  // directory.
  //
  // Note that leaving the directory in place in case of a submission error
  // would have prevent the user from re-submitting until we research the
  // issue and manually remove the directory.
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

  // Run the submission handler, if specified, reading the result manifest
  // from its stdout and caching it as a name/value pair list for later use
  // (forwarding to the client, sending via email, etc.). Otherwise, create
  // implied result manifest.
  //
  status_code sc;
  vector<manifest_name_value> rvs;

  if (options_->ci_handler_specified ())
  {
    using namespace external_handler;

    optional<result_manifest> r (run (options_->ci_handler (),
                                      options_->ci_handler_argument (),
                                      dd,
                                      options_->ci_handler_timeout (),
                                      error,
                                      warn,
                                      verb_ ? &trace : nullptr));
    if (!r)
    {
      stash_submit_dir ();
      return respond_error (); // The diagnostics is already issued.
    }

    sc = r->status;
    rvs = move (r->values);
  }
  else // Create the implied result manifest.
  {
    sc = 200;

    auto add = [&rvs] (string n, string v)
    {
      manifest_name_value nv {move (n), move (v),
                              0 /* name_line */,  0 /* name_column */,
                              0 /* value_line */, 0 /* value_column */};

      rvs.emplace_back (move (nv));
    };

    add ("", "1");                           // Start of manifest.
    add ("status", "200");
    add ("message", "CI request is queued");
    add ("reference", request_id);
    add ("", "");                            // End of manifest.
  }

  assert (!rvs.empty ()); // Produced by the handler or is implied.

  // Serialize the submission result manifest to a stream. On the
  // serialization error log the error description and return false, on the
  // stream error pass through the io_error exception, otherwise return true.
  //
  auto rsm = [&rvs, &error, &request_id] (ostream& os) -> bool
  {
    try
    {
      serializer s (os, "result");
      for (const manifest_name_value& nv: rvs)
        s.next (nv.name, nv.value);

      return true;
    }
    catch (const serialization& e)
    {
      error << "ref " << request_id << ": unable to serialize handler's "
            << "output: " << e;
      return false;
    }
  };

  // If the submission data directory still exists then perform an appropriate
  // action on it, depending on the submission result status. Note that the
  // handler could move or remove the directory.
  //
  if (dir_exists (dd))
  {
    // Remove the directory if the client error is detected.
    //
    if (sc >= 400 && sc < 500)
      rmdir_r (dd);

    // Otherwise, save the result manifest, into the directory. Also stash the
    // directory for troubleshooting in case of the server error.
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

      if (sc >= 500 && sc < 600)
        stash_submit_dir ();
    }
  }

  // Send email, if configured, and the CI request submission is not simulated.
  //
  // Note that we don't consider the email sending failure to be a submission
  // failure as the submission data is successfully persisted and the handler
  // is successfully executed, if configured. One can argue that email can be
  // essential for the submission processing and missing it would result in
  // the incomplete submission. In this case it's natural to assume that the
  // web server error log is monitored and the email sending failure will be
  // noticed.
  //
  if (options_->ci_email_specified () && simulate.empty ())
  try
  {
    // Redirect the diagnostics to the web server error log.
    //
    sendmail sm ([&trace, this] (const char* args[], size_t n)
                 {
                   l2 ([&]{trace << process_args {args, n};});
                 },
                 2 /* stderr */,
                 options_->email (),
                 "CI request submission (" + request_id + ")",
                 {options_->ci_email ()});

    // Write the submission request manifest.
    //
    bool r (rqm (sm.out));
    assert (r); // The serialization succeeded once, so can't fail now.

    // Write the submission result manifest.
    //
    sm.out << "\n\n";

    rsm (sm.out); // We don't care about the result (see above).

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

  if (!rsm (rs.content (sc, "text/manifest;charset=utf-8")))
    return respond_error (); // The error description is already logged.

  return true;
}
