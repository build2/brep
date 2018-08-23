// file      : mod/mod-submit.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-submit.hxx>

#include <sys/time.h>   // timeval
#include <sys/select.h>

#include <ratio>        // ratio_greater_equal
#include <chrono>
#include <cstdlib>      // strtoul()
#include <istream>
#include <sstream>
#include <type_traits>  // static_assert
#include <system_error> // error_code, generic_category()

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
  string ref; // Will be set later.
  auto respond_manifest = [&rs, &ref] (status_code status,
                                       const string& message) -> bool
  {
    serializer s (rs.content (status, "text/manifest;charset=utf-8"),
                  "response");

    s.next ("", "1");                      // Start of manifest.
    s.next ("status", to_string (status));
    s.next ("message", message);

    if (!ref.empty ())
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

  // Note that from now on the result manifest will contain the reference
  // value.
  //
  ref = string (sha256sum, 0, 12);

  // Check for a duplicate submission.
  //
  // Respond with the unprocessable entity (422) code if a duplicate is found.
  //
  dir_path dd (options_->submit_data () / dir_path (ref));

  if (dir_exists (dd) || simulate == "duplicate-archive")
    return respond_manifest (422, "duplicate submission");

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
                   dir_path (path::traits::temp_name (ref)));

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

    // Respond with the unprocessable entity (422) code for the archive
    // checksum mismatch.
    //
    if (sha.string () != sha256sum)
      return respond_manifest (422, "package archive checksum mismatch");
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

  auto rqm = [&a, &sha256sum, &ts, &simulate, &rq, &rps, &respond_manifest]
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
        if (n != "archive" && n != "sha256sum" && n != "simulate")
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
  // Respond with the unprocessable entity (422) code if a submission race is
  // detected.
  //
  try
  {
    mvdir (td, dd);
  }
  catch (const system_error& e)
  {
    int ec (e.code ().value ());
    if (ec == ENOTEMPTY || ec == EEXIST)
      return respond_manifest (422, "duplicate submission");

    error << "unable to rename directory '" << td << "' to '" << dd << "': "
          << e;

    return respond_error ();
  }

  // Given that the submission data is now successfully persisted we are no
  // longer in charge of removing it, except for the cases when the submission
  // handler terminates with an error (see below for details).
  //
  tdr.cancel ();

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
    try
    {
      if (!dir_exists (dd))
        return;

      for (size_t n (1); true; ++n) // Eventually we should find the free one.
      {
        string ext (".fail." + to_string (n));
        dir_path d (dd + ext);

        if (!dir_exists (d))
        try
        {
          mvdir (dd, d);
          break;
        }
        catch (const system_error& e)
        {
          int ec (e.code ().value ());
          if (ec != ENOTEMPTY && ec != EEXIST) // Note: there can be a race.
            throw;
        }
      }
    }
    catch (const system_error& e)
    {
      // Not much we can do here. Let's just log the issue and bail out
      // leaving the directory in place.
      //
      error << "unable to rename directory '" << dd << "': " << e;
    }
  };

  auto print_args = [&trace, this] (const char* args[], size_t n)
  {
    l2 ([&]{trace << process_args {args, n};});
  };

  // Run the submission handler, if specified, reading the result manifest
  // from its stdout and caching it as a name/value pair list for later use
  // (forwarding to the client, sending via email, etc.). Otherwise, create
  // implied result manifest.
  //
  status_code sc (200);
  vector<manifest_name_value> rvs;

  if (options_->submit_handler_specified ())
  {
    // For the sake of the documentation we will call the handler's normal
    // exit with 0 code "successful termination".
    //
    // To make sure the handler process execution doesn't exceed the specified
    // timeout we set the non-blocking mode for the process stdout-reading
    // stream, try to read from it with the 10 milliseconds timeout and check
    // the process execution time between the reads. We then kill the process
    // if the execution time is exceeded.
    //
    using namespace chrono;

    using time_point = system_clock::time_point;
    using duration   = system_clock::duration;

    // Make sure that the system clock has at least milliseconds resolution.
    //
    static_assert(
      ratio_greater_equal<milliseconds::period, duration::period>::value,
      "The system clock resolution is too low");

    optional<milliseconds> timeout;

    if (options_->submit_handler_timeout_specified ())
      timeout = milliseconds (options_->submit_handler_timeout () * 1000);

    const path& handler (options_->submit_handler ());

    // Note that due to the non-blocking mode we cannot just pass the stream
    // to the manifest parser constructor. So we buffer the data in the string
    // stream and then parse that.
    //
    stringstream ss;

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

      auto kill = [&pr, &warn, &handler, &ref] ()
      {
        // We may still end up well (see below), thus this is a warning.
        //
        warn << "ref " << ref << ": process " << handler
             << " execution timeout expired";

        pr.kill ();
      };

      try
      {
        ifdstream is (move (pipe.in), fdstream_mode::non_blocking);

        const size_t nbuf (8192);
        char buf[nbuf];

        while (is.is_open ())
        {
          time_point start;
          milliseconds wd (10); // Max time to wait for the data portion.

          if (timeout)
          {
            start = system_clock::now ();

            if (*timeout < wd)
              wd = *timeout;
          }

          timeval tm {wd.count () / 1000        /* seconds */,
                      wd.count () % 1000 * 1000 /* microseconds */};

          fd_set rd;
          FD_ZERO (&rd);
          FD_SET  (is.fd (), &rd);

          int r (select (is.fd () + 1, &rd, nullptr, nullptr, &tm));

          if (r == -1)
          {
            // Don't fail if the select() call was interrupted by the signal.
            //
            if (errno != EINTR)
              throw_system_ios_failure (errno, "select failed");
          }
          else if (r != 0) // Is data available?
          {
            assert (FD_ISSET (is.fd (), &rd));

            // The only leagal way to read from non-blocking ifdstream.
            //
            streamsize n (is.readsome (buf, nbuf));

            // Close the stream (and bail out) if the end of the data is
            // reached. Otherwise cache the read data.
            //
            if (is.eof ())
              is.close ();
            else
            {
              // The data must be available.
              //
              // Note that we could keep reading until the readsome() call
              // returns 0. However, this way we could potentially exceed the
              // timeout significantly for some broken handler that floods us
              // with data. So instead, we will be checking the process
              // execution time after every data chunk read.
              //
              assert (n != 0);

              ss.write (buf, n);
            }
          }
          else // Timeout occured.
          {
            // Normally, we don't expect timeout to occur on the pipe read
            // operation if the process has terminated successfully, as all its
            // output must already be buffered (including eof). However, there
            // can be some still running handler's child that has inherited
            // the parent's stdout. In this case we assume that we have read
            // all the handler's output, close the stream, log the warning and
            // bail out.
            //
            if (pr.exit)
            {
              // We keep reading only upon successful handler termination.
              //
              assert (*pr.exit);

              is.close ();

              warn << "ref " << ref << ": process " << handler
                   << " stdout is not closed after termination (possibly "
                   << "handler's child still running)";
            }
          }

          if (timeout)
          {
            time_point now (system_clock::now ());

            // Assume we have waited the full amount if the time adjustment is
            // detected.
            //
            duration d (now > start ? now - start : wd);

            // If the timeout is not fully exhausted, then decrement it and
            // try to read some more data from the handler' stdout. Otherwise,
            // kill the process, if not done yet.
            //
            // Note that it may happen that we are killing an already
            // terminated process, in which case kill() just sets the process
            // exit information. On the other hand it's guaranteed that the
            // process is terminated after the kill() call, and so the pipe is
            // presumably closed on the write end (see above for details).
            // Thus, if the process terminated successfully, we will continue
            // reading until eof is reached or read timeout occurred. Yes, it
            // may happen that we end up with a successful submission even
            // with the kill.
            //
            if (*timeout > d)
              *timeout -= duration_cast<milliseconds> (d);
            else if (!pr.exit)
            {
              kill ();

              assert (pr.exit);

              // Close the stream (and bail out) if the process hasn't
              // terminate successfully.
              //
              if (!*pr.exit)
                is.close ();

              *timeout = milliseconds::zero ();
            }
          }
        }

        assert (!is.is_open ());

        if (!timeout)
          pr.wait ();

        // If the process is not terminated yet, then wait for its termination
        // for the remaining time. Kill it if the timeout has been exceeded
        // and the process still hasn't terminate.
        //
        else if (!pr.exit && !pr.timed_wait (*timeout))
          kill ();

        assert (pr.exit); // The process must finally be terminated.

        if (*pr.exit)
          break; // Get out of the breakout loop.

        error << "ref " << ref << ": process " << handler << " " << *pr.exit;

        // Fall through.
      }
      catch (const io_error& e)
      {
        if (pr.wait ())
          error << "ref " << ref << ": unable to read handler's output: " << e;

        // Fall through.
      }

      stash_submit_dir ();
      return respond_error ();
    }
    // Handle process_error and io_error (both derive from system_error).
    //
    catch (const system_error& e)
    {
      error << "unable to execute '" << handler << "': " << e;

      stash_submit_dir ();
      return respond_error ();
    }

    try
    {
      // Parse and verify the manifest. Obtain the HTTP status code (must go
      // first) and cache it for the subsequent response to the client.
      //
      parser p (ss, "handler");
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
        bad_value ("invalid HTTP status '" + v + "'");

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
    }
    catch (const parsing& e)
    {
      error << "ref " << ref << ": unable to parse handler's output: " << e;

      // It appears the handler had misbehaved, so let's stash the submission
      // directory for troubleshooting.
      //
      stash_submit_dir ();

      return respond_error ();
    }
  }
  else // Create implied result manifest.
  {
    auto add = [&rvs] (string n, string v)
    {
      manifest_name_value nv {move (n), move (v),
                              0 /* name_line */,  0 /* name_column */,
                              0 /* value_line */, 0 /* value_column */};

      rvs.emplace_back (move (nv));
    };

    add ("", "1");                           // Start of manifest.
    add ("status", "200");
    add ("message", "submission is queued");
    add ("reference", ref);
    add ("", "");                            // End of manifest.
  }

  assert (!rvs.empty ()); // Produced by the handler or is implied.

  // Serialize the submission result manifest to a stream. On the
  // serialization error log the error description and return false, on the
  // stream error pass through the io_error exception, otherwise return true.
  //
  auto rsm = [&rvs, &error, &ref] (ostream& os) -> bool
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
      error << "ref " << ref << ": unable to serialize handler's output: " << e;
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
                 "new package submission " + a.string () + " (" + ref + ")",
                 {options_->submit_email ()});

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
