// file      : mod/mod-ci.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-ci.hxx>

#include <libbutl/fdstream.hxx>
#include <libbutl/manifest-parser.hxx>
#include <libbutl/manifest-serializer.hxx>

#include <libbpkg/manifest.hxx>     // package_manifest
#include <libbpkg/package-name.hxx>

#include <web/server/module.hxx>

#include <web/xhtml/serialization.hxx>

#include <mod/page.hxx>
#include <mod/module-options.hxx>

using namespace std;
using namespace butl;
using namespace web;
using namespace brep::cli;

// ci
//
#ifdef BREP_CI_TENANT_SERVICE
brep::ci::
ci (tenant_service_map& tsm)
    : tenant_service_map_ (tsm)
{
}
#endif

brep::ci::
#ifdef BREP_CI_TENANT_SERVICE
ci (const ci& r, tenant_service_map& tsm)
#else
ci (const ci& r)
#endif
    :
#ifndef BREP_CI_TENANT_SERVICE_UNLOADED
      handler (r),
  #else
      database_module (r),
#endif
      ci_start (r),
      options_ (r.initialized_ ? r.options_ : nullptr),
      form_ (r.initialized_ || r.form_ == nullptr
             ? r.form_
             : make_shared<xhtml::fragment> (*r.form_))
#ifdef BREP_CI_TENANT_SERVICE
    , tenant_service_map_ (tsm)
#endif
{
}

void brep::ci::
init (scanner& s)
{
  HANDLER_DIAG;

#ifdef BREP_CI_TENANT_SERVICE
  {
    shared_ptr<tenant_service_base> ts (
      dynamic_pointer_cast<tenant_service_base> (shared_from_this ()));

    assert (ts != nullptr); // By definition.

    tenant_service_map_["ci"] = move (ts);
  }
#endif

  options_ = make_shared<options::ci> (
    s, unknown_mode::fail, unknown_mode::fail);

  // Prepare for the CI requests handling, if configured.
  //
  if (options_->ci_data_specified ())
  {
    ci_start::init (make_shared<options::ci_start> (*options_));

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

#ifdef BREP_CI_TENANT_SERVICE_UNLOADED
    if (!options_->build_config_specified ())
      fail << "package building functionality must be enabled";

    database_module::init (*options_, options_->build_db_retry ());
#endif

    if (options_->root ().empty ())
      options_->root (dir_path ("/"));
  }
}

bool brep::ci::
handle (request& rq, response& rs)
{
  using namespace bpkg;
  using namespace xhtml;

  using parser        = manifest_parser;
  using parsing       = manifest_parsing;
  using serializer    = manifest_serializer;
  using serialization = manifest_serialization;

  HANDLER_DIAG;

  // We will respond with the manifest to the CI request submission protocol
  // violations and with a plain text message on the internal errors. In the
  // latter case we will always respond with the same neutral message for
  // security reason, logging the error details. Note that descriptions of
  // exceptions caught by the web server are returned to the client (see
  // web/server/module.hxx for details), and we want to avoid this when there
  // is a danger of exposing sensitive data.
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
                                 const string& message) -> bool
  {
    serializer s (rs.content (status, "text/manifest;charset=utf-8"),
                  "response");

    s.next ("", "1");                      // Start of manifest.
    s.next ("status", to_string (status));
    s.next ("message", message);
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

  const dir_path& root (options_->root ());

  // Parse the request form data and verify the submission size limit.
  //
  // Note that the submission may include the overrides upload that we don't
  // expect to be large.
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

  // Verify the remote repository location.
  //
  const repository_location rl (params.repository ());

  if (rl.empty () || rl.local ())
    return respond_manifest (400, "invalid repository location");

  // Parse the package name[/version] arguments.
  //
  vector<package> packages;

  for (const string& s: params.package ())
  {
    //  Let's skip the potentially unfilled package form fields.
    //
    if (s.empty ())
      continue;

    try
    {
      package pkg;
      size_t p (s.find ('/'));

      if (p != string::npos)
      {
        pkg.name = package_name (string (s, 0, p));

        // Not to confuse with module::version.
        //
        pkg.version = bpkg::version (string (s, p + 1));
      }
      else
        pkg.name = package_name (s);

      packages.push_back (move (pkg));
    }
    catch (const invalid_argument&)
    {
      return respond_manifest (400, "invalid package " + s);
    }
  }

  // Verify that unknown parameter values satisfy the requirements (contain
  // only UTF-8 encoded graphic characters plus '\t', '\r', and '\n') and
  // stash them.
  //
  // Actually, the expected ones must satisfy too, so check them as well.
  //
  vector<pair<string, string>> custom_request;
  {
    string what;
    for (const name_value& nv: rps)
    {
      if (nv.value &&
          !utf8 (*nv.value, what, codepoint_types::graphic, U"\n\r\t"))
        return respond_manifest (400,
                                 "invalid parameter " + nv.name + ": " + what);

      const string& n (nv.name);

      if (n != "repository"  &&
          n != "_"           &&
          n != "package"     &&
          n != "overrides"   &&
          n != "interactive" &&
          n != "simulate")
         custom_request.emplace_back (n, nv.value ? *nv.value : "");
    }
  }

  // Parse and validate overrides, if present.
  //
  vector<pair<string, string>> overrides;

  if (params.overrides_specified ())
  try
  {
    istream& is (rq.open_upload ("overrides"));
    parser mp (is, "overrides");
    vector<manifest_name_value> ovrs (parse_manifest (mp));

    package_manifest::validate_overrides (ovrs, mp.name ());

    overrides.reserve (ovrs.size ());
    for (manifest_name_value& nv: ovrs)
      overrides.emplace_back (move (nv.name), move (nv.value));
  }
  // Note that invalid_argument (thrown by open_upload() function call) can
  // mean both no overrides upload or multiple overrides uploads.
  //
  catch (const invalid_argument&)
  {
    return respond_manifest (400, "overrides upload expected");
  }
  catch (const parsing& e)
  {
    return respond_manifest (400,
                             string ("unable to parse overrides: ") +
                             e.what ());
  }
  catch (const io_error& e)
  {
    error << "unable to read overrides: " << e;
    return respond_error ();
  }

  // Stash the User-Agent HTTP header and the client IP address.
  //
  optional<string> client_ip;
  optional<string> user_agent;
  for (const name_value& h: rq.headers ())
  {
    if (icasecmp (h.name, ":Client-IP") == 0)
      client_ip = h.value;
    else if (icasecmp (h.name, "User-Agent") == 0)
      user_agent = h.value;
  }

#ifndef BREP_CI_TENANT_SERVICE_UNLOADED
  optional<start_result> r (start (error,
                                   warn,
                                   verb_ ? &trace : nullptr,
#ifdef BREP_CI_TENANT_SERVICE
                                   tenant_service ("", "ci"),
#else
                                   nullopt /* service */,
#endif
                                   rl,
                                   packages,
                                   client_ip,
                                   user_agent,
                                   (params.interactive_specified ()
                                    ? params.interactive ()
                                    : optional<string> ()),
                                   (!simulate.empty ()
                                    ? simulate
                                    : optional<string> ()),
                                   custom_request,
                                   overrides));
#else
  assert (build_db_ != nullptr); // Wouldn't be here otherwise.

  optional<start_result> r;

  if (optional<pair<string, duplicate_tenant_result>> ref =
      create (error,
              warn,
              verb_ ? &trace : nullptr,
              *build_db_, retry_,
              tenant_service ("", "ci", rl.string ()),
              chrono::seconds (40),
              chrono::seconds (10)))
  {
    string msg ("unloaded CI request is created: " +
                options_->host () + tenant_dir (root, ref->first).string ());

    r = start_result {200, move (msg), move (ref->first), {}};
  }
#endif

  if (!r)
    return respond_error (); // The diagnostics is already issued.

  try
  {
    serialize_manifest (*r,
                        rs.content (r->status, "text/manifest;charset=utf-8"));
  }
  catch (const serialization& e)
  {
    error << "ref " << r->reference << ": unable to serialize handler's "
          << "output: " << e;

    return respond_error ();
  }

  return true;
}

#ifdef BREP_CI_TENANT_SERVICE
function<optional<string> (const string& tenant_id,
                           const brep::tenant_service&)> brep::ci::
build_queued (const string& /*tenant_id*/,
              const tenant_service&,
              const vector<build>& bs,
              optional<build_state> initial_state,
              const build_queued_hints& hints,
              const diag_epilogue& log_writer) const noexcept
{
  NOTIFICATION_DIAG (log_writer);

  l2 ([&]{trace << "initial_state: "
                << (initial_state ? to_string (*initial_state) : "none")
                << ", hints "
                << hints.single_package_version << ' '
                << hints.single_package_config;});

  return [&bs, initial_state] (const string& tenant_id,
                               const tenant_service& ts)
         {
           optional<string> r (ts.data);

           for (const build& b: bs)
           {
             string s ((!initial_state
                        ? "queued "
                        : "queued " + to_string (*initial_state) + ' ') +
                       tenant_id + '/'                                  +
                       b.package_name.string () + '/'                   +
                       b.package_version.string () + '/'                +
                       b.target.string () + '/'                         +
                       b.target_config_name + '/'                       +
                       b.package_config_name + '/'                      +
                       b.toolchain_name + '/'                           +
                       b.toolchain_version.string ());

             if (r)
             {
               *r += ", ";
               *r += s;
             }
             else
               r = move (s);
           }

           return r;
         };
}

function<optional<string> (const string& tenant_id,
                           const brep::tenant_service&)> brep::ci::
build_building (const string& /*tenant_id*/,
                const tenant_service&,
                const build& b,
                const diag_epilogue&) const noexcept
{
  return [&b] (const string& tenant_id,
               const tenant_service& ts)
         {
           string s ("building "                       +
                     tenant_id + '/'                   +
                     b.package_name.string () + '/'    +
                     b.package_version.string () + '/' +
                     b.target.string () + '/'          +
                     b.target_config_name + '/'        +
                     b.package_config_name + '/'       +
                     b.toolchain_name + '/'            +
                     b.toolchain_version.string ());

           return ts.data ? *ts.data + ", " + s : s;
         };
}

function<pair<optional<string>, bool> (const string& tenant_id,
                                       const brep::tenant_service&)> brep::ci::
build_built (const string& /*tenant_id*/,
             const tenant_service&,
             const build& b,
             const diag_epilogue&) const noexcept
{
  return [&b] (const string& tenant_id, const tenant_service& ts)
         {
           string s ("built "                          +
                     tenant_id + '/'                   +
                     b.package_name.string () + '/'    +
                     b.package_version.string () + '/' +
                     b.target.string () + '/'          +
                     b.target_config_name + '/'        +
                     b.package_config_name + '/'       +
                     b.toolchain_name + '/'            +
                     b.toolchain_version.string ());

           return make_pair (
             optional<string> (ts.data ? *ts.data + ", " + s : s), false);
         };
}

#ifdef BREP_CI_TENANT_SERVICE_UNLOADED
function<optional<string> (const string& tenant_id,
                           const brep::tenant_service&)> brep::ci::
build_unloaded (const string& /* tenant_id */,
                tenant_service&& ts,
                const diag_epilogue& log_writer) const noexcept
{
  NOTIFICATION_DIAG (log_writer);

  assert (ts.data); // Repository location.

  try
  {
    repository_location rl (*ts.data);

    if (!load (error, warn, verb_ ? &trace : nullptr,
               *build_db_, retry_,
               move (ts),
               rl))
      return nullptr; // The diagnostics is already issued.
  }
  catch (const invalid_argument& e)
  {
    error << "invalid repository location '" << *ts.data << "' stored for "
          << "tenant service " << ts.id << ' ' << ts.type;

    return nullptr;
  }

  return [] (const string& tenant_id, const tenant_service& ts)
         {
           return "loaded " + tenant_id + ' ' + *ts.data;
         };
}
#endif
#endif

// ci_cancel
//
brep::ci_cancel::
ci_cancel (const ci_cancel& r)
    : database_module (r),
      options_ (r.initialized_ ? r.options_ : nullptr)
{
}

void brep::ci_cancel::
init (scanner& s)
{
  options_ = make_shared<options::ci_cancel> (
    s, unknown_mode::fail, unknown_mode::fail);

  if (options_->build_config_specified ())
    database_module::init (*options_, options_->build_db_retry ());
}

bool brep::ci_cancel::
handle (request& rq, response& rs)
{
  HANDLER_DIAG;

  if (build_db_ == nullptr)
    throw invalid_request (501, "not implemented");

  params::ci_cancel params;

  try
  {
    name_value_scanner s (rq.parameters (1024));
    params = params::ci_cancel (s, unknown_mode::fail, unknown_mode::fail);
  }
  catch (const cli::exception& e)
  {
    throw invalid_request (400, e.what ());
  }

  const string& reason (params.reason ());

  if (reason.empty ())
    throw invalid_request (400, "missing CI request cancellation reason");

  // Verify the tenant id.
  //
  const string tid (params.id ());

  if (tid.empty ())
    throw invalid_request (400, "invalid CI request id");

  if (!cancel (error, warn, verb_ ? &trace : nullptr,
               reason,
               *build_db_, retry_,
               tid))
    throw invalid_request (400, "unknown CI request id");

  // We have all the data, so don't buffer the response content.
  //
  ostream& os (rs.content (200, "text/plain;charset=utf-8", false));
  os << "CI request " << tid << " has been canceled";

  return true;
}
