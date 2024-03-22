// file      : mod/mod-build-force.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-build-force.hxx>

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <web/server/module.hxx>

#include <libbrep/build.hxx>
#include <libbrep/build-odb.hxx>

#include <mod/module-options.hxx>
#include <mod/tenant-service.hxx>

using namespace std;
using namespace brep::cli;
using namespace odb::core;

brep::build_force::
build_force (const tenant_service_map& tsm)
    : tenant_service_map_ (tsm)
{
}

// While currently the user-defined copy constructor is not required (we don't
// need to deep copy nullptr's), it is a good idea to keep the placeholder
// ready for less trivial cases.
//
brep::build_force::
build_force (const build_force& r, const tenant_service_map& tsm)
    : database_module (r),
      build_config_module (r),
      options_ (r.initialized_ ? r.options_ : nullptr),
      tenant_service_map_ (tsm)
{
}

void brep::build_force::
init (scanner& s)
{
  HANDLER_DIAG;

  options_ = make_shared<options::build_force> (
    s, unknown_mode::fail, unknown_mode::fail);

  if (options_->build_config_specified ())
  {
    database_module::init (*options_, options_->build_db_retry ());
    build_config_module::init (*options_);
  }
}

bool brep::build_force::
handle (request& rq, response& rs)
{
  using brep::version; // Not to confuse with module::version.

  HANDLER_DIAG;

  if (build_db_ == nullptr)
    throw invalid_request (501, "not implemented");

  params::build_force params;

  try
  {
    name_value_scanner s (rq.parameters (8 * 1024));
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

  try
  {
    package_name p;

    try
    {
      p = package_name (move (params.package ()));
    }
    catch (const invalid_argument& e)
    {
      throw invalid_argument (string ("invalid package name: ") + e.what ());
    }

    // We accept the non-url-encoded version representation. Note that the
    // parameter is already url-decoded by the web server, so we just restore
    // the space character (that is otherwise forbidden in version
    // representation) to the plus character.
    //
    // @@ Move to types-parsers.hxx?
    //
    auto parse_version = [] (string& v, const char* what) -> version
    {
      replace (v.begin (), v.end (), ' ', '+');

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

    version package_version (parse_version (params.version (),
                                            "package version"));

    target_triplet target;

    try
    {
      target = target_triplet (params.target ());
    }
    catch (const invalid_argument& e)
    {
      throw invalid_argument (string ("invalid target: ") + e.what ());
    }

    string& target_config (params.target_config ());

    if (target_config.empty ())
      throw invalid_argument ("no target configuration name");

    string& package_config (params.package_config ());

    if (package_config.empty ())
      throw invalid_argument ("no package configuration name");

    string& toolchain_name (params.toolchain_name ());

    if (toolchain_name.empty ())
      throw invalid_argument ("no toolchain name");

    version toolchain_version (parse_version (params.toolchain_version (),
                                              "toolchain version"));

    id = build_id (package_id (move (tenant), move (p), package_version),
                   move (target),
                   move (target_config),
                   move (package_config),
                   move (toolchain_name),
                   toolchain_version);
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
  if (target_conf_map_->find (
        build_target_config_id {id.target,
                                id.target_config_name}) ==
      target_conf_map_->end ())
    config_expired ("no target configuration");

  // Load the package build configuration (if present), set the force flag and
  // update the object's persistent state.
  //
  // If the incomplete package build is being forced to rebuild and the
  // tenant_service_build_queued callback is associated with the package
  // tenant, then stash the state, the build object, and the callback pointer
  // for the subsequent service `queued` notification.
  //
  const tenant_service_build_queued* tsq (nullptr);
  optional<pair<tenant_service, shared_ptr<build>>> tss;

  connection_ptr conn (build_db_->connection ());
  {
    transaction t (conn->begin ());

    package_build pb;
    shared_ptr<build> b;

    if (!build_db_->query_one<package_build> (
          query<package_build>::build::id == id, pb) ||
        (b = move (pb.build))->state == build_state::queued)
      config_expired ("no package build");

    force_state force (b->state == build_state::built
                       ? force_state::forced
                       : force_state::forcing);

    if (b->force != force)
    {
      // Log the force rebuild with the warning severity, truncating the
      // reason if too long.
      //
      diag_record dr (warn);
      dr << "force rebuild for ";

      if (!b->tenant.empty ())
        dr << b->tenant << ' ';

      dr << b->package_name << '/' << b->package_version << ' '
         << b->target_config_name << '/' << b->target << ' '
         << b->package_config_name << ' '
         << b->toolchain_name << '-' << b->toolchain_version
         << " (state: " << to_string (b->state) << ' ' << to_string (b->force)
         << "): ";

      if (reason.size () < 50)
        dr << reason;
      else
        dr << string (reason, 0, 50) << "...";

      b->force = force;
      build_db_->update (b);

      if (force == force_state::forcing)
      {
        shared_ptr<build_tenant> t (build_db_->load<build_tenant> (b->tenant));

        if (t->service)
        {
          auto i (tenant_service_map_.find (t->service->type));

          if (i != tenant_service_map_.end ())
          {
            tsq = dynamic_cast<const tenant_service_build_queued*> (
              i->second.get ());

            if (tsq != nullptr)
            {
              // If we ought to call the
              // tenant_service_build_queued::build_queued() callback, then
              // also set the package tenant's queued timestamp to the current
              // time to prevent the notifications race (see
              // tenant::queued_timestamp for details).
              //
              t->queued_timestamp = system_clock::now ();
              build_db_->update (t);

              tss = make_pair (move (*t->service), move (b));
            }
          }
        }
      }
    }

    t.commit ();
  }

  // If the incomplete package build is being forced to rebuild and the
  // tenant-associated third-party service needs to be notified about the
  // queued builds, then call the tenant_service_build_queued::build_queued()
  // callback function and update the service state, if requested.
  //
  if (tsq != nullptr)
  {
    assert (tss); // Wouldn't be here otherwise.

    const tenant_service& ss (tss->first);
    build& b (*tss->second);

    vector<build> qbs;
    qbs.push_back (move (b));

    if (auto f = tsq->build_queued (ss, qbs, build_state::building, log_writer_))
      update_tenant_service_state (conn, qbs.back ().tenant, f);
  }

  // We have all the data, so don't buffer the response content.
  //
  ostream& os (rs.content (200, "text/plain;charset=utf-8", false));
  os << "Rebuilding in " << options_->build_forced_rebuild_timeout ()
     << " seconds.";

  return true;
}
