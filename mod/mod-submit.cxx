// file      : mod/mod-submit.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-submit.hxx>

#include <cstdlib> // strtoul()
#include <istream>

#include <libbutl/sha256.mxx>
#include <libbutl/process.mxx>
#include <libbutl/sendmail.mxx>
#include <libbutl/fdstream.mxx>
#include <libbutl/timestamp.mxx>
#include <libbutl/filesystem.mxx>
#include <libbutl/process-io.mxx>          // operator<<(ostream, process_args)
#include <libbutl/manifest-parser.mxx>
#include <libbutl/manifest-serializer.mxx>

#include <web/xhtml.hxx>
#include <web/module.hxx>

#include <mod/page.hxx>
#include <mod/options.hxx>

using namespace std;
using namespace butl;
using namespace web;
using namespace brep::cli;

brep::submit::
submit (const submit& r)
    : handler (r),
      options_ (r.initialized_ ? r.options_ : nullptr),
      form_ (r.initialized_ || r.form_ == nullptr
             ? r.form_
             : make_shared<xhtml::fragment> (*r.form_))
{
}

void brep::submit::
init (scanner& s)
{
  HANDLER_DIAG;

  options_ = make_shared<options::submit> (
    s, unknown_mode::fail, unknown_mode::fail);

  // Verify that the submission handling is setup properly, if configured.
  //
  if (options_->submit_data_specified ())
  {
    // Verify that directories satisfy the requirements.
    //
    auto verify = [&fail] (const dir_path& d, const char* what)
    {
      if (d.relative ())
        fail << what << " directory path must be absolute";

      if (!dir_exists (d))
        fail << what << " directory '" << d << "' does not exist";
    };

    verify (options_->submit_data (), "submit-data");
    verify (options_->submit_temp (), "submit-temp");

    // Parse XHTML5 form file, if configured.
    //
    if (options_->submit_form_specified ())
    {
      const path& submit_form (options_->submit_form ());

      if (submit_form.relative ())
        fail << "submit-form path must be absolute";

      try
      {
        ifdstream is (submit_form);

        form_ = make_shared<xhtml::fragment> (is.read_text (),
                                              submit_form.string ());
      }
      catch (const xml::parsing& e)
      {
        fail << "unable to parse submit-form file: " << e;
      }
      catch (const io_error& e)
      {
        fail << "unable to read submit-form file '" << submit_form << "': "
             << e;
      }
    }

    if (options_->submit_handler_specified () &&
        options_->submit_handler ().relative ())
      fail << "submit-handler path must be absolute";
  }

  if (options_->root ().empty ())
    options_->root (dir_path ("/"));
}

bool brep::submit::
handle (request& rq, response& rs)
{
  using namespace xhtml;

  using parser        = manifest_parser;
  using parsing       = manifest_parsing;
  using serializer    = manifest_serializer;
  using serialization = manifest_serialization;

  HANDLER_DIAG;

  const dir_path& root (options_->root ());

  // We will respond with the manifest to the submission protocol violations
  // and with a plain text message on the internal errors. In the latter case
  // we will always respond with the same neutral message for security reason,
  // logging the error details. Note that descriptions of exceptions caught by
  // the web server are returned to the client (see web/module.hxx for
  // details), and we want to avoid this when there is a danger of exposing
  // sensitive data.
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
  auto respond_manifest = [&rs] (status_code status,
                                 const string& message,
                                 const char* ref = nullptr) -> bool
  {
    serializer s (rs.content (status, "text/manifest;charset=utf-8"),
                  "response");

    s.next ("", "1");                      // Start of manifest.
    s.next ("status", to_string (status));
    s.next ("message", message);

    if (ref != nullptr)
      s.next ("reference", ref);

    s.next ("", ""); // End of manifest.
    return true;
  };

  auto respond_error = [&rs] (status_code status = 500) -> bool
  {
    rs.content (status, "text/plain;charset=utf-8")
      << "submission handling failed" << endl;

    return true;
  };

  // Check if the package submission functionality is enabled.
  //
  // Note that this is not a submission protocol violation but it feels right
  // to respond with the manifest, to help the client a bit.
  //
  if (!options_->submit_data_specified ())
    return respond_manifest (404, "submission disabled");

  // Parse the request form data and verifying the submission size limit.
  //
  // Note that if it is exceeded, then there are parameters and this is the
  // submission rather than the form request, and so we respond with the
  // manifest.
  //
  try
  {
    rq.parameters (options_->submit_max_size ());
  }
  catch (const invalid_request& e)
  {
    if (e.status == 413) // Payload too large?
      return respond_manifest (e.status, "submission size exceeds limit");

    throw;
  }

  // The request parameters are now parsed and the limit doesn't really matter.
  //
  const name_values& rps (rq.parameters (0 /* limit */));

  // If there is no request parameters then we respond with the submission
  // form XHTML, if configured. Otherwise, will proceed as for the submission
  // request and will fail (missing parameters).
  //
  if (rps.empty () && form_ != nullptr)
  {
    const string title ("Submit");

    xml::serializer s (rs.content (), title);

    s << HTML
      <<   HEAD
      <<     TITLE << title << ~TITLE
      <<     CSS_LINKS (path ("submit.css"), root)
      <<   ~HEAD
      <<   BODY
      <<     DIV_HEADER (root, options_->logo (), options_->menu ())
      <<     DIV(ID="content") << *form_ << ~DIV
      <<   ~BODY
      << ~HTML;

    return true;
  }

  // Verify the submission parameters we expect. The unknown ones will be
  // serialized to the submission manifest.
  //
  params::submit params;

  try
  {
    name_value_scanner s (rps);
    params = params::submit (s, unknown_mode::skip, unknown_mode::skip);
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
      <<   BODY << "submission handling failed" << ~BODY
      << ~HTML;

    return true;
  }

  const string& archive (params.archive ());
  const string& sha256sum (params.sha256sum ());

  if (archive.empty ())
    return respond_manifest (400, "package archive expected");

  if (sha256sum.empty ())
    return respond_manifest (400, "package archive checksum expected");

  if (sha256sum.size () != 64)
    return respond_manifest (400, "invalid package archive checksum");

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

  // Check for a duplicate submission.
  //
  // Respond with the conflict (409) code if a duplicate is found.
  //
  string ac (sha256sum, 0, 12);
  dir_path dd (options_->submit_data () / dir_path (ac));

  if (dir_exists (dd) || simulate == "duplicate-archive")
    return respond_manifest (409, "duplicate submission");

  // Create the temporary submission directory.
  //
  dir_path td;

  try
  {
    // Note that providing a meaningful prefix for temp_name() is not really
    // required as the temporary directory is used by brep exclusively. However,
    // using the abbreviated checksum can be helpful for troubleshooting.
    //
    td = dir_path (options_->submit_temp () /
                   dir_path (path::traits::temp_name (ac)));

    // It's highly unlikely but still possible that the temporary directory
    // already exists. This can only happen due to the unclean web server
    // shutdown. Let's remove it and retry.
    //
    if (try_mkdir (td) == mkdir_status::already_exists)
    {
      try_rmdir_r (td);

      if (try_mkdir (td) == mkdir_status::already_exists)
        throw_generic_error (EEXIST);
    }
  }
  catch (const invalid_path&)
  {
    return respond_manifest (400, "invalid package archive checksum");
  }
  catch (const system_error& e)
  {
    error << "unable to create directory '" << td << "': " << e;
    return respond_error ();
  }

  auto_rmdir tdr (td);

  // Save the package archive into the temporary directory and verify its
  // checksum.
  //
  // Note that the archive file name can potentially contain directory path
  // in the client's form (e.g., Windows), so let's strip it if that's the
  // case.
  //
  path a;
  path af;

  try
  {
    size_t n (archive.find_last_of ("\\/"));
    a = path (n != string::npos ? string (archive, n + 1) : archive);
    af = td / a;
  }
  catch (const invalid_path&)
  {
    return respond_manifest (400, "invalid package archive name");
  }

  try
  {
    istream& is (rq.open_upload ("archive"));

    // Note that istream::read() sets failbit if unable to read the requested
    // number of bytes.
    //
    is.exceptions (istream::badbit);

    sha256 sha;
    char buf[8192];
    ofdstream os (af, ios::binary);

    while (!eof (is))
    {
      is.read (buf, sizeof (buf));

      if (size_t n = is.gcount ())
      {
        sha.append (buf, n);
        os.write (buf, n);
      }
    }

    os.close ();

    if (sha.string () != sha256sum)
      return respond_manifest (400, "package archive checksum mismatch");
  }
  // Note that invalid_argument (thrown by open_upload() function call) can
  // mean both no archive upload or multiple archive uploads.
  //
  catch (const invalid_argument&)
  {
    return respond_manifest (400, "package archive upload expected");
  }
  catch (const io_error& e)
  {
    error << "unable to write package archive '" << af << "': " << e;
    return respond_error ();
  }

  // Serialize the submission request manifest to a stream. On the
  // serialization error respond to the client with the manifest containing
  // the bad request (400) code and return false, on the stream error pass
  // through the io_error exception, otherwise return true.
  //
  timestamp ts (system_clock::now ());

  auto rqm = [&a, &sha256sum, &ts, &rq, &rps, &respond_manifest]
             (ostream& os) -> bool
  {
    try
    {
      serializer s (os, "request");

      // Serialize the submission manifest header.
      //
      s.next ("", "1");                // Start of manifest.
      s.next ("archive", a.string ());
      s.next ("sha256sum", sha256sum);

      s.next ("timestamp",
              butl::to_string (ts,
                               "%Y-%m-%dT%H:%M:%SZ",
                               false /* special */,
                               false /* local */));

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
        if (n != "archive" && n != "sha256sum")
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

  // Serialize the submission request manifest to the temporary submission
  // directory.
  //
  path rqf (td / "request.manifest");

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

  // Make the temporary submission directory permanent.
  //
  // Respond with the conflict (409) code if a submission race is detected.
  //
  try
  {
    mvdir (td, dd);
  }
  catch (const system_error& e)
  {
    int ec (e.code ().value ());
    if (ec == ENOTEMPTY || ec == EEXIST)
      return respond_manifest (409, "duplicate submission");

    error << "unable to rename directory '" << td << "' to '" << dd << "': "
          << e;

    return respond_error ();
  }

  // Given that the submission data is now successfully persisted we are no
  // longer in charge of removing it, even in case of a subsequent error.
  //
  tdr.cancel ();

  auto print_args = [&trace, this] (const char* args[], size_t n)
  {
    l2 ([&]{trace << process_args {args, n};});
  };

  // Run the submission handler, if specified, reading the result manifest
  // from its stdout and caching it as a name/value pair list for later use
  // (forwarding to the client, sending via email, etc.).
  //
  // Note that if the handler is configured then the cache can never be empty,
  // containing at least the status value. Thus, an empty cache indicates that
  // the handler is not configured.
  //
  status_code sc;
  vector<manifest_name_value> rvs;

  if (options_->submit_handler_specified ())
  {
    const path& handler (options_->submit_handler ());

    for (;;) // Breakout loop.
    try
    {
      fdpipe pipe (fdopen_pipe ()); // Can throw io_error.

      // Redirect the diagnostics to the web server error log.
      //
      process pr (
        process_start_callback (print_args,
                                0     /* stdin  */,
                                pipe  /* stdout */,
                                2     /* stderr */,
                                handler,
                                options_->submit_handler_argument (),
                                dd));
      pipe.out.close ();

      try
      {
        ifdstream is (move (pipe.in));

        // Parse and verify the manifest. Obtain the HTTP status code (must go
        // first) and cache it for the subsequent responding to the client.
        //
        parser p (is, "handler");
        manifest_name_value nv (p.next ());

        auto bad_value ([&p, &nv] (const string& d) {
            throw parsing (p.name (), nv.value_line, nv.value_column, d);});

        if (nv.empty ())
          bad_value ("empty manifest");

        const string& n (nv.name);
        const string& v (nv.value);

        // The format version pair is verified by the parser.
        //
        assert (n.empty () && v == "1");

        // Cache the format version pair.
        //
        rvs.push_back (move (nv));

        // Get and verify the HTTP status.
        //
        nv = p.next ();
        if (n != "status")
          bad_value ("no status specified");

        char* e (nullptr);
        unsigned long c (strtoul (v.c_str (), &e, 10)); // Can't throw.

        assert (e != nullptr);

        if (!(*e == '\0' && c >= 100 && c < 600))
          bad_value ("invalid http status '" + v + "'");

        // Cache the HTTP status.
        //
        sc = static_cast<status_code> (c);
        rvs.push_back (move (nv));

        // Cache the remaining name/value pairs.
        //
        for (nv = p.next (); !nv.empty (); nv = p.next ())
          rvs.push_back (move (nv));

        // Cache end of manifest.
        //
        rvs.push_back (move (nv));

        is.close ();

        if (pr.wait ())
          break; // Get out of the breakout loop.

        assert (pr.exit);
        error << "process " << handler << " " << *pr.exit;

        // Fall through.
      }
      catch (const parsing& e)
      {
        if (pr.wait ())
          error << "unable to parse handler's output: " << e;

        // Fall through.
      }
      catch (const io_error& e)
      {
        if (pr.wait ())
          error << "unable to read handler's output: " << e;

        // Fall through.
      }

      return respond_error ();
    }
    // Handle process_error and io_error (both derive from system_error).
    //
    catch (const system_error& e)
    {
      error << "unable to execute '" << handler << "': " << e;
      return respond_error ();
    }
  }

  // Serialize the submission result manifest to a stream. On the
  // serialization error log the error description and return false, on the
  // stream error pass through the io_error exception, otherwise return true.
  //
  auto rsm = [&rvs, &error] (ostream& os) -> bool
  {
    assert (!rvs.empty ());

    try
    {
      serializer s (os, "result");
      for (const manifest_name_value& nv: rvs)
        s.next (nv.name, nv.value);

      return true;
    }
    catch (const serialization& e)
    {
      error << "unable to serialize handler's output: " << e;
      return false;
    }
  };

  // Save the result manifest, if generated, into the submission directory
  // if it still exists (note that the handler could move or remove it).
  //
  path rsf (dd / "result.manifest");

  if (!rvs.empty () && dir_exists (dd))
  try
  {
    ofdstream os (rsf);
    bool r (rsm (os));
    os.close ();

    if (!r)
      return respond_error (); // The error description is already logged.
  }
  catch (const io_error& e)
  {
    error << "unable to write to '" << rsf << "': " << e;
    return respond_error ();
  }

  // Send email, if configured, and the submission is not simulated.
  //
  // Note that we don't consider the email sending failure to be a submission
  // failure as the submission data is successfully persisted and the handler
  // is successfully executed, if configured. One can argue that email can be
  // essential for the submission processing and missing it would result in
  // the incomplete submission. In this case it's natural to assume that the
  // web server error log is monitored and the email sending failure will be
  // noticed.
  //
  if (options_->submit_email_specified () && simulate.empty ())
  try
  {
    // Redirect the diagnostics to the web server error log.
    //
    sendmail sm (print_args,
                 2 /* stderr */,
                 options_->email (),
                 "new package submission " + a.string () + " (" + ac + ")",
                 {options_->submit_email ()});

    // Write the submission request manifest.
    //
    bool r (rqm (sm.out));
    assert (r); // The serialization succeeded once, so can't fail now.

    // Write the submission result manifest, if present.
    //
    if (!rvs.empty ())
    {
      sm.out << "\n\n";

      rsm (sm.out); // We don't care about the result (see above).
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

  // Respond with implied result manifest if the handler is not configured.
  //
  if (rvs.empty ())
    return respond_manifest (200, "submission queued", ac.c_str ());

  if (!rsm (rs.content (sc, "text/manifest;charset=utf-8")))
    return respond_error (); // The error description is already logged.

  return true;
}
