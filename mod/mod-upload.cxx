// file      : mod/mod-upload.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-upload.hxx>

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <libbutl/uuid.hxx>
#include <libbutl/base64.hxx>
#include <libbutl/sha256.hxx>
#include <libbutl/sendmail.hxx>
#include <libbutl/fdstream.hxx>
#include <libbutl/timestamp.hxx>
#include <libbutl/filesystem.hxx>
#include <libbutl/process-io.hxx>          // operator<<(ostream, process_args)
#include <libbutl/manifest-types.hxx>
#include <libbutl/manifest-serializer.hxx>

#include <web/server/module.hxx>

#include <libbrep/build.hxx>
#include <libbrep/build-odb.hxx>
#include <libbrep/build-package.hxx>
#include <libbrep/build-package-odb.hxx>

#include <mod/module-options.hxx>
#include <mod/external-handler.hxx>

using namespace std;
using namespace butl;
using namespace brep::cli;
using namespace odb::core;

// While currently the user-defined copy constructor is not required (we don't
// need to deep copy nullptr's), it is a good idea to keep the placeholder
// ready for less trivial cases.
//
brep::upload::
upload (const upload& r)
    : build_result_module (r),
      options_ (r.initialized_ ? r.options_  : nullptr)
{
}

void brep::upload::
init (scanner& s)
{
  HANDLER_DIAG;

  options_ = make_shared<options::upload> (
    s, unknown_mode::fail, unknown_mode::fail);

  // Verify that the upload handling is setup properly, if configured.
  //
  for (const auto& ud: options_->upload_data ())
  {
    const string& t (ud.first);

    if (t.empty ())
      fail << "empty upload type in upload-data configuration option";

    if (ud.second.relative ())
      fail << t << " upload-data path '" << ud.second << "' is relative";

    if (!dir_exists (ud.second))
      fail << t << " upload-data directory '" << ud.second
           << "' does not exist";

    const map<string, path>& uh (options_->upload_handler ());
    auto i (uh.find (t));

    if (i != uh.end () && i->second.relative ())
      fail << t << " upload-handler path '" << i->second << "' is relative";
  }

  if (options_->upload_data_specified ())
  {
    if (!options_->build_config_specified ())
      fail << "upload functionality is enabled but package building "
           << "functionality is disabled";

    build_result_module::init (*options_, *options_);
  }

  if (options_->root ().empty ())
    options_->root (dir_path ("/"));
}

bool brep::upload::
handle (request& rq, response& rs)
{
  using brep::version; // Not to confuse with module::version.

  using serializer    = manifest_serializer;
  using serialization = manifest_serialization;

  HANDLER_DIAG;

  // We will respond with the manifest to the upload protocol violations and
  // with a plain text message on the internal errors. In the latter case we
  // will always respond with the same neutral message for security reason,
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

    s.next ("", ""); // End of manifest.
    return true;
  };

  auto respond_error = [&rs] (status_code status = 500) -> bool
  {
    rs.content (status, "text/plain;charset=utf-8")
      << "upload handling failed" << endl;

    return true;
  };

  // Check if the upload functionality is enabled.
  //
  // Note that this is not an upload protocol violation but it feels right to
  // respond with the manifest, to help the client a bit.
  //
  if (!options_->upload_data_specified ())
    return respond_manifest (404, "upload disabled");

  // Parse the request data and verify the upload size limit.
  //
  // Note that the size limit is upload type-specific. Thus, first, we need to
  // determine the upload type which we expect to be specified in the URL as a
  // value of the upload parameter.
  //
  string type;
  dir_path dir;

  try
  {
    name_value_scanner s (rq.parameters (0 /* limit */, true /* url_only */));

    // We only expect the upload=<type> parameter in URL.
    //
    params::upload params (
      params::upload (s, unknown_mode::fail, unknown_mode::fail));

    type = move (params.type ());

    if (type.empty ())
      return respond_manifest (400, "upload type expected");

    // Check if this upload type is enabled. While at it, cache the upload
    // data directory path.
    //
    const map<string, dir_path>& ud (options_->upload_data ());
    auto i (ud.find (type));

    if (i == ud.end ())
      return respond_manifest (404, type + " upload disabled");

    dir = i->second;
  }
  catch (const cli::exception&)
  {
    return respond_manifest (400, "invalid parameter");
  }

  try
  {
    const map<string, size_t>& us (options_->upload_max_size ());
    auto i (us.find (type));
    rq.parameters (i != us.end () ? i->second : 10485760); // 10M by default.
  }
  catch (const invalid_request& e)
  {
    if (e.status == 413) // Payload too large?
      return respond_manifest (e.status, type + " upload size exceeds limit");

    throw;
  }

  // The request parameters are now parsed and the limit doesn't really matter.
  //
  const name_values& rps (rq.parameters (0 /* limit */));

  // Verify the upload parameters we expect. The unknown ones will be
  // serialized to the upload manifest.
  //
  params::upload params;

  try
  {
    name_value_scanner s (rps);
    params = params::upload (s, unknown_mode::skip, unknown_mode::skip);
  }
  catch (const cli::exception&)
  {
    return respond_manifest (400, "invalid parameter");
  }

  const string& session   (params.session ());
  const string& instance  (params.instance ());
  const string& archive   (params.archive ());
  const string& sha256sum (params.sha256sum ());

  if (session.empty ())
    return respond_manifest (400, "upload session expected");

  optional<vector<char>> challenge;

  if (params.challenge_specified ())
  try
  {
    challenge = base64_decode (params.challenge ());
  }
  catch (const invalid_argument&)
  {
    return respond_manifest (400, "invalid challenge");
  }

  if (instance.empty ())
    return respond_manifest (400, "upload instance expected");

  if (archive.empty ())
    return respond_manifest (400, "upload archive expected");

  if (sha256sum.empty ())
    return respond_manifest (400, "upload archive checksum expected");

  if (sha256sum.size () != 64)
    return respond_manifest (400, "invalid upload archive checksum");

  // Verify that unknown parameter values satisfy the requirements (contain
  // only UTF-8 encoded graphic characters plus '\t', '\r', and '\n').
  //
  // Actually, the expected ones must satisfy too, so check them as well.
  //
  string what;
  for (const name_value& nv: rps)
  {
    if (nv.value &&
        !utf8 (*nv.value, what, codepoint_types::graphic, U"\n\r\t"))
      return respond_manifest (400,
                               "invalid parameter " + nv.name + ": " + what);
  }

  parse_session_result sess;

  try
  {
    sess = parse_session (session);
  }
  catch (const invalid_argument& e)
  {
    return respond_manifest (400, string ("invalid session: ") + e.what ());
  }

  // If the session expired (no such configuration, etc) then, similar to the
  // build result module, we log this case with the warning severity and
  // respond with manifest with the 200 status as if the session is valid (see
  // the build result module for the reasoning).
  //
  auto warn_expired = [&session, &warn] (const string& d)
  {
    warn << "session '" << session << "' expired: " << d;
  };

  const build_id& id (sess.id);

  // Make sure the build configuration still exists.
  //
  const build_target_config* tc;
  {
    auto i (target_conf_map_->find (
              build_target_config_id {id.target, id.target_config_name}));

    if (i == target_conf_map_->end ())
    {
      warn_expired ("no build configuration");
      return respond_manifest (200, type + " upload is queued");
    }

    tc = i->second;
  }

  // Note that if the session authentication fails (probably due to the
  // authentication settings change), then we log this case with the warning
  // severity and respond with manifest with the 200 status as if the
  // challenge is valid (see the build result module for the reasoning).
  //
  shared_ptr<build> bld;
  shared_ptr<build_package> pkg;
  shared_ptr<build_repository> rep;
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
    else if (b->timestamp != sess.timestamp)
    {
      warn_expired ("non-matching timestamp");
    }
    else if (authenticate_session (*options_, challenge, *b, session))
    {
      bld = move (b);
      pkg = build_db_->load<build_package> (id.package);
      rep = pkg->internal_repository.load ();
    }

    t.commit ();
  }

  // Note that from now on the result manifest we respond with will contain
  // the reference value.
  //
  try
  {
    request_id = uuid::generate ().string ();
  }
  catch (const system_error& e)
  {
    error << "unable to generate request id: " << e;
    return respond_error ();
  }

  if (bld == nullptr)
    return respond_manifest (200, type + " upload is queued");

  // Create the upload data directory.
  //
  dir_path dd (dir / dir_path (request_id));

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

  // Save the package archive into the temporary directory and verify its
  // checksum.
  //
  // Note that the archive file name can potentially contain directory path in
  // the POSIX form, so let's strip it if that's the case.
  //
  path a;
  path af;

  try
  {
    size_t n (archive.find_last_of ('/'));
    a = path (n != string::npos ? string (archive, n + 1) : archive);
    af = dd / a;
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
    ofdstream os (af, fdopen_mode::binary);

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
      return respond_manifest (422, "upload archive checksum mismatch");
  }
  // Note that invalid_argument (thrown by open_upload() function call) can
  // mean both no archive upload or multiple archive uploads.
  //
  catch (const invalid_argument&)
  {
    return respond_manifest (400, "archive upload expected");
  }
  catch (const io_error& e)
  {
    error << "unable to write package archive '" << af << "': " << e;
    return respond_error ();
  }

  // Serialize the upload request manifest to a stream. On the serialization
  // error respond to the client with the manifest containing the bad request
  // (400) code and return false, on the stream error pass through the
  // io_error exception, otherwise return true.
  //
  timestamp ts (system_clock::now ());

  auto rqm = [&request_id,
              &ts,
              &rps,
              &session,
              &instance,
              &a,
              &sha256sum,
              &id,
              &bld,
              &pkg,
              &rep,
              &tc,
              &sess,
              &respond_manifest,
              this] (ostream& os, bool long_lines = false) -> bool
  {
    try
    {
      serializer s (os, "request", long_lines);

      // Serialize the upload manifest header.
      //
      s.next ("", "1");                // Start of manifest.
      s.next ("id", request_id);
      s.next ("session", session);
      s.next ("instance", instance);
      s.next ("archive", a.string ());
      s.next ("sha256sum", sha256sum);

      s.next ("timestamp",
              butl::to_string (ts,
                               "%Y-%m-%dT%H:%M:%SZ",
                               false /* special */,
                               false /* local */));

      s.next ("name", id.package.name.string ());
      s.next ("version", pkg->version.string ());
      s.next ("project", pkg->project.string ());
      s.next ("target-config", tc->name);
      s.next ("package-config", id.package_config_name);
      s.next ("target", tc->target.string ());

      if (!tenant.empty ())
        s.next ("tenant", tenant);

      s.next ("toolchain-name", id.toolchain_name);
      s.next ("toolchain-version", sess.toolchain_version.string ());
      s.next ("repository-name", rep->canonical_name);

      s.next ("machine-name", bld->machine);
      s.next ("machine-summary", bld->machine_summary);

      // Serialize the request parameters.
      //
      // Note that the serializer constraints the parameter names (can't start
      // with '#', can't contain ':' and the whitespaces, etc.).
      //
      for (const name_value& nv: rps)
      {
        // Note that the upload parameter is renamed to '_' by the root
        // handler (see the request_proxy class for details).
        //
        const string& n (nv.name);
        if (n != "_"         &&
            n != "session"   &&
            n != "challenge" &&
            n != "instance"  &&
            n != "archive"   &&
            n != "sha256sum")
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

  // Serialize the upload request manifest to the upload directory.
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

  // Given that the upload data is now successfully persisted we are no longer
  // in charge of removing it, except for the cases when the upload
  // handler terminates with an error (see below for details).
  //
  ddr.cancel ();

  // If the handler terminates with non-zero exit status or specifies 5XX
  // (HTTP server error) upload result manifest status value, then we stash
  // the upload data directory for troubleshooting. Otherwise, if it's the 4XX
  // (HTTP client error) status value, then we remove the directory.
  //
  auto stash_upload_dir = [&dd, error] ()
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

  // Run the upload handler, if specified, reading the result manifest from
  // its stdout and caching it as a name/value pair list for later use
  // (forwarding to the client, sending via email, etc). Otherwise, create
  // implied result manifest.
  //
  status_code sc;
  vector<manifest_name_value> rvs;

  const map<string, path>& uh (options_->upload_handler ());
  auto hi (uh.find (type));

  if (hi != uh.end ())
  {
    auto range (options_->upload_handler_argument ().equal_range (type));

    strings args;
    for (auto i (range.first); i != range.second; ++i)
      args.push_back (i->second);

    const map<string, size_t>& ht (options_->upload_handler_timeout ());
    auto i (ht.find (type));

    optional<external_handler::result_manifest> r (
      external_handler::run (hi->second,
                             args,
                             dd,
                             i != ht.end () ? i->second : 0,
                             error,
                             warn,
                             verb_ ? &trace : nullptr));

    if (!r)
    {
      stash_upload_dir ();
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
      manifest_name_value nv {
        move (n), move (v),
        0 /* name_line */,  0 /* name_column */,
        0 /* value_line */, 0 /* value_column */,
        0 /* start_pos */, 0 /* colon_pos */, 0 /* end_pos */};

      rvs.emplace_back (move (nv));
    };

    add ("status", "200");
    add ("message", type + " upload is queued");
    add ("reference", request_id);
  }

  assert (!rvs.empty ()); // Produced by the handler or is implied.

  // Serialize the upload result manifest to a stream. On the serialization
  // error log the error description and return false, on the stream error
  // pass through the io_error exception, otherwise return true.
  //
  auto rsm = [&rvs,
              &error,
              &request_id,
              &type] (ostream& os, bool long_lines = false) -> bool
  {
    try
    {
      serializer s (os, "result", long_lines);
      serialize_manifest (s, rvs);
      return true;
    }
    catch (const serialization& e)
    {
      error << "ref " << request_id << ": unable to serialize " << type
            << " upload handler's output: " << e;
      return false;
    }
  };

  // If the upload data directory still exists then perform an appropriate
  // action on it, depending on the upload result status. Note that the
  // handler could move or remove the directory.
  //
  if (dir_exists (dd))
  {
    // Remove the directory if the client error is detected.
    //
    if (sc >= 400 && sc < 500)
    {
      rmdir_r (dd);
    }
    //
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
        // claim the upload failed. The error is logged nevertheless.
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
        stash_upload_dir ();
    }
  }

  // Send email, if configured. Use the long lines manifest serialization mode
  // for the convenience of copying/clicking URLs they contain.
  //
  // Note that we don't consider the email sending failure to be an upload
  // failure as the upload data is successfully persisted and the handler is
  // successfully executed, if configured. One can argue that email can be
  // essential for the upload processing and missing it would result in the
  // incomplete upload. In this case it's natural to assume that the web
  // server error log is monitored and the email sending failure will be
  // noticed.
  //
  const map<string, string>& ue (options_->upload_email ());
  auto ei (ue.find (type));

  if (ei != ue.end ())
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
                 type + " upload (" + request_id + ')',
                 {ei->second});

    // Write the upload request manifest.
    //
    bool r (rqm (sm.out, true /* long_lines */));
    assert (r); // The serialization succeeded once, so can't fail now.

    // Write the upload result manifest.
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

  if (!rsm (rs.content (sc, "text/manifest;charset=utf-8")))
    return respond_error (); // The error description is already logged.

  return true;
}
