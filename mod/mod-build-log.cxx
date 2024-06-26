// file      : mod/mod-build-log.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-build-log.hxx>

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <libbutl/timestamp.hxx> // to_stream()

#include <web/server/module.hxx>

#include <libbrep/build.hxx>
#include <libbrep/build-odb.hxx>

#include <mod/module-options.hxx>

using namespace std;
using namespace brep::cli;
using namespace odb::core;

// While currently the user-defined copy constructor is not required (we don't
// need to deep copy nullptr's), it is a good idea to keep the placeholder
// ready for less trivial cases.
//
brep::build_log::
build_log (const build_log& r)
    : database_module (r),
      build_config_module (r),
      options_ (r.initialized_ ? r.options_ : nullptr)
{
}

void brep::build_log::
init (scanner& s)
{
  options_ = make_shared<options::build_log> (
    s, unknown_mode::fail, unknown_mode::fail);

  if (options_->build_config_specified ())
  {
    database_module::init (*options_, options_->build_db_retry ());
    build_config_module::init (*options_);
  }

  if (options_->root ().empty ())
    options_->root (dir_path ("/"));
}

bool brep::build_log::
handle (request& rq, response& rs)
{
  using brep::version; // Not to confuse with module::version.

  HANDLER_DIAG;

  if (build_db_ == nullptr)
    throw invalid_request (501, "not implemented");

  // Parse the HTTP request URL path (without the root directory) to obtain
  // the build id and optional operation name. If the operation is not
  // specified then print logs for all the operations.
  //
  // Note that the URL path must be in the following form:
  //
  // <pkg-name>/<pkg-version>/log/<cfg-name>/<target>/<toolchain-name>/<toolchain-version>[/<operation>]
  //
  // Also note that the presence of the first 3 components is guaranteed by
  // the repository_root module.
  //
  build_id id;
  string op;

  path lpath (rq.path ().leaf (options_->root ()));

  // If the tenant is not empty then it is contained in the leftmost path
  // component (see repository_root for details). Strip it if that's the case.
  //
  if (!tenant.empty ())
  {
    assert (!lpath.empty ());
    lpath = path (++lpath.begin (), lpath.end ());
  }

  assert (!lpath.empty ());

  try
  {
    auto i (lpath.begin ());

    package_name name;
    try
    {
      name = package_name (*i++);
    }
    catch (const invalid_argument& e)
    {
      throw invalid_argument (string ("invalid package name: ") + e.what ());
    }

    assert (i != lpath.end ());

    auto parse_version = [] (const string& v, const char* what) -> version
    {
      // Intercept exception handling to add the parsing error attribution.
      //
      try
      {
        return brep::version (v);
      }
      catch (const invalid_argument& e)
      {
        throw invalid_argument (string ("invalid ") + what + ": " + e.what ());
      }
    };

    version package_version (parse_version (*i++, "package version"));

    assert (i != lpath.end () && *i == "log");

    if (++i == lpath.end ())
      throw invalid_argument ("no target");

    target_triplet target;
    try
    {
      target = target_triplet (*i++);
    }
    catch (const invalid_argument& e)
    {
      throw invalid_argument (string ("invalid target: ") + e.what ());
    }

    if (i == lpath.end ())
      throw invalid_argument ("no target configuration name");

    string target_config (*i++);

    if (target_config.empty ())
      throw invalid_argument ("empty target configuration name");

    if (i == lpath.end ())
      throw invalid_argument ("no package configuration name");

    string package_config (*i++);

    if (package_config.empty ())
      throw invalid_argument ("empty package configuration name");

    if (i == lpath.end ())
      throw invalid_argument ("no toolchain name");

    string toolchain_name (*i++);

    if (toolchain_name.empty ())
      throw invalid_argument ("empty toolchain name");

    if (i == lpath.end ())
      throw invalid_argument ("no toolchain version");

    version toolchain_version (parse_version (*i++, "toolchain version"));

    id = build_id (package_id (tenant, move (name), package_version),
                   move (target),
                   move (target_config),
                   move (package_config),
                   move (toolchain_name),
                   toolchain_version);

    if (i != lpath.end ())
      op = *i++;

    if (i != lpath.end ())
      throw invalid_argument ("unexpected path component");
  }
  catch (const invalid_argument& e)
  {
    throw invalid_request (400, e.what ());
  }

  // Make sure no parameters passed.
  //
  try
  {
    name_value_scanner s (rq.parameters (1024));
    params::build_log (s, unknown_mode::fail, unknown_mode::fail);
  }
  catch (const cli::exception& e)
  {
    throw invalid_request (400, e.what ());
  }

  // If the package build configuration expired (no such configuration,
  // package, etc), then we log this case with the trace severity and respond
  // with the 404 HTTP code (not found but may be available in the future).
  // The thinking is that this may be or may not be a problem with the
  // controller's setup (expires too fast or the link from some ancient email
  // is opened).
  //
  auto config_expired = [&trace, &lpath, this] (const string& d)
  {
    l2 ([&]{trace << "package build configuration for " << lpath
                  << (!tenant.empty () ? '(' + tenant + ')' : "")
                  << " expired: " << d;});

    throw invalid_request (404, "package build configuration expired: " + d);
  };

  // Make sure the build configuration still exists.
  //
  if (target_conf_map_->find (
        build_target_config_id {id.target,
                                id.target_config_name}) ==
      target_conf_map_->end ())
    config_expired ("no target configuration");

  // Load the package build configuration (if present).
  //
  shared_ptr<build> b;
  {
    transaction t (build_db_->begin ());

    package_build pb;
    if (!build_db_->query_one<package_build> (
          query<package_build>::build::id == id, pb))
      config_expired ("no package build");

    b = move (pb.build);
    if (b->state != build_state::built)
    {
      config_expired ("state is " + to_string (b->state));
    }
    else
    {
      build_db_->load (*b, b->results_section);
      build_db_->load (*b, b->auxiliary_machines_section);
    }

    t.commit ();
  }

  // We have all the data so don't buffer the response content.
  //
  // Note that after we started to write the response content we need to be
  // accurate not throwing any exceptions, that would mess up the response.
  //
  ostream& os (rs.content (200, "text/plain;charset=utf-8", false));

  auto print_header = [&os, &b, this] ()
  {
    // Print the build tenant in the multi-tenant mode.
    //
    if (!b->tenant.empty ())
      os << options_->tenant_name () << ": " << b->tenant << endl << endl;

    os << "package:           " << b->package_name           << endl
       << "version:           " << b->package_version        << endl
       << "toolchain:         " << b->toolchain_name << '-'
                                << b->toolchain_version      << endl
       << "target:            " << b->target << endl
       << "target config:     " << b->target_config_name     << endl
       << "package config:    " << b->package_config_name    << endl
       << "build machine:     " << b->machine.name << " -- "
                                << b->machine.summary        << endl;

    for (const build_machine& m: b->auxiliary_machines)
      os << "auxiliary machine: " << m.name << " -- " << m.summary << endl;

    os << "timestamp:         ";

    butl::to_stream (os,
                     b->timestamp,
                     "%Y-%m-%d %H:%M:%S%[.N] %Z",
                     true /* special */,
                     true /* local */);

    os << endl << endl;
  };

  if (op.empty ())
  {
    print_header ();

    for (const auto& r: b->results)
      os << r.operation << ": " << r.status << endl;

    os << endl;

    for (const auto& r: b->results)
      os << r.log;
  }
  else
  {
    const operation_results& r (b->results);

    auto i (
      find_if (r.begin (), r.end (),
               [&op] (const operation_result& v) {return v.operation == op;}));

    if (i == r.end ())
      config_expired ("no operation");

    print_header ();

    os << op << ": " << i->status << endl << endl
       << i->log;
  }

  return true;
}
