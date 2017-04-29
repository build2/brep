// file      : mod/mod-build-force.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-build-force.hxx>

#include <algorithm> // replace()

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <web/module.hxx>

#include <libbrep/build.hxx>
#include <libbrep/build-odb.hxx>
#include <libbrep/package.hxx>
#include <libbrep/package-odb.hxx>

#include <mod/options.hxx>

using namespace std;
using namespace bbot;
using namespace brep::cli;
using namespace odb::core;

// While currently the user-defined copy constructor is not required (we don't
// need to deep copy nullptr's), it is a good idea to keep the placeholder
// ready for less trivial cases.
//
brep::build_force::
build_force (const build_force& r)
    : database_module (r),
      options_ (r.initialized_ ? r.options_ : nullptr)
{
}

void brep::build_force::
init (scanner& s)
{
  MODULE_DIAG;

  options_ = make_shared<options::build_force> (
    s, unknown_mode::fail, unknown_mode::fail);

  database_module::init (static_cast<options::package_db> (*options_),
                         options_->package_db_retry ());

  if (options_->build_config_specified ())
    database_module::init (static_cast<options::build>    (*options_),
                           static_cast<options::build_db> (*options_),
                           options_->build_db_retry ());
}

bool brep::build_force::
handle (request& rq, response& rs)
{
  using brep::version; // Not to confuse with module::version.

  MODULE_DIAG;

  if (build_db_ == nullptr)
    throw invalid_request (501, "not implemented");

  params::build_force params;

  try
  {
    name_value_scanner s (rq.parameters ());
    params = params::build_force (s, unknown_mode::fail, unknown_mode::fail);
  }
  catch (const cli::exception& e)
  {
    throw invalid_request (400, e.what ());
  }

  const string& reason (params.reason ());

  if (reason.empty ())
    throw invalid_request (400, "missing rebuild reason");

  build_id id;
  version version; // Keep for logging.

  try
  {
    string& p (params.package ());

    if (p.empty ())
      throw invalid_argument ("empty package name");

    // We accept the non-url-encoded version representation. Note that the
    // parameter is already url-decoded by the web server, so we just restore
    // the space character (that is otherwise forbidden in version
    // representation) to the plus character.
    //
    string& v (params.version ());
    replace (v.begin (), v.end (), ' ', '+');

    // Intercept exception handling to add the parsing error attribution.
    //
    try
    {
      version = brep::version (v);
    }
    catch (const invalid_argument& e)
    {
      throw invalid_argument (
        string ("invalid package version: ") + e.what ());
    }

    string& c (params.configuration ());

    if (c.empty ())
      throw invalid_argument ("no configuration name");

    id = build_id (package_id (move (p), version), move (c));
  }
  catch (const invalid_argument& e)
  {
    throw invalid_request (400, e.what ());
  }

  // If the package build configuration expired (no such configuration,
  // package, etc), then we respond with the 404 HTTP code (not found but may
  // be available in the future).
  //
  auto config_expired = [] (const string& d)
  {
    throw invalid_request (404, "package build configuration expired: " + d);
  };

  // Make sure the build configuration still exists.
  //
  auto i (
    find_if (
      build_conf_->begin (), build_conf_->end (),
      [&id] (const build_config& c) {return c.name == id.configuration;}));

  if (i == build_conf_->end ())
    config_expired ("no configuration");

  // Make sure the package still exists.
  //
  {
    transaction t (package_db_->begin ());
    shared_ptr<package> p (package_db_->find<package> (id.package));
    t.commit ();

    if (p == nullptr)
      config_expired ("no package");
  }

  // Load the package build configuration (if present), set the force flag and
  // update the object's persistent state.
  //
  {
    transaction t (build_db_->begin ());
    shared_ptr<build> b (build_db_->find<build> (id));

    if (b == nullptr)
      config_expired ("no package configuration");

    // Respond with 409 (conflict) if the package configuration is in
    // inappropriate state for being rebuilt.
    //
    else if (b->state != build_state::tested)
      throw invalid_request (409, "state is " + to_string (b->state));

    if (!b->forced)
    {
      b->forced = true;
      build_db_->update (b);

      l1 ([&]{trace << "force rebuild for "
                    << id.package.name << '/' << version << ' '
                    << id.configuration << ": " << reason;});
    }

    t.commit ();
  }

  // We have all the data, so don't buffer the response content.
  //
  ostream& os (rs.content (200, "text/plain;charset=utf-8", false));
  os << "Rebuilding in " << options_->build_forced_rebuild_timeout ()
     << " seconds.";

  return true;
}
