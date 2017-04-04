// file      : mod/mod-build-task.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-build-task>

#include <map>
#include <chrono>

#include <butl/utility>             // compare_c_string
#include <butl/filesystem>          // path_match()
#include <butl/manifest-parser>
#include <butl/manifest-serializer>

#include <bbot/manifest>
#include <bbot/build-config>

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <web/module>

#include <brep/build>
#include <brep/build-odb>
#include <brep/package>
#include <brep/package-odb>

#include <mod/options>

using namespace std;
using namespace butl;
using namespace bbot;
using namespace brep::cli;
using namespace odb::core;

// While currently the user-defined copy constructor is not required (we don't
// need to deep copy nullptr's), it is a good idea to keep the placeholder
// ready for less trivial cases.
//
brep::build_task::
build_task (const build_task& r)
    : database_module (r),
      options_ (r.initialized_ ? r.options_ : nullptr)
{
}

void brep::build_task::
init (scanner& s)
{
  MODULE_DIAG;

  options_ = make_shared<options::build_task> (
    s, unknown_mode::fail, unknown_mode::fail);

  database_module::init (static_cast<options::package_db> (*options_),
                         options_->package_db_retry ());

  if (options_->build_config_specified ())
    database_module::init (static_cast<options::build>    (*options_),
                           static_cast<options::build_db> (*options_),
                           options_->build_db_retry ());

  if (options_->root ().empty ())
    options_->root (dir_path ("/"));
}

bool brep::build_task::
handle (request& rq, response& rs)
{
  MODULE_DIAG;

  if (build_db_ == nullptr)
    throw invalid_request (501, "not implemented");

  // Make sure no parameters passed.
  //
  try
  {
    name_value_scanner s (rq.parameters ());
    params::build_task (s, unknown_mode::fail, unknown_mode::fail);
  }
  catch (const cli::exception& e)
  {
    throw invalid_request (400, e.what ());
  }

  task_request_manifest tqm;

  try
  {
    size_t limit (options_->build_task_request_max_size ());
    manifest_parser p (rq.content (limit, limit), "task_request_manifest");
    tqm = task_request_manifest (p);
  }
  catch (const manifest_parsing& e)
  {
    throw invalid_request (400, e.what ());
  }

  task_response_manifest tsm;

  // Map build configurations to machines that are capable of building them.
  // The first matching machine is selected for each configuration. Also
  // create the configuration name list for use in database queries.
  //
  struct config_machine
  {
    const build_config* config;
    const machine_header_manifest* machine;
  };

  using config_machines = map<const char*, config_machine, compare_c_string>;

  cstrings cfg_names;
  config_machines cfg_machines;

  for (const auto& c: *build_conf_)
  {
    for (auto& m: tqm.machines)
    {
      if (path_match (c.machine_pattern, m.name) &&
          cfg_machines.insert (
            make_pair (c.name.c_str (), config_machine ({&c, &m}))).second)
        cfg_names.push_back (c.name.c_str ());
    }
  }

  // Go through packages until we find one that has no build configuration
  // present in the database, or has the untested one, or in the testing state
  // but expired, or the one, which build failed abnormally and expired. If
  // such a package configuration is found then put it into the testing state,
  // set the current timestamp and respond with the task for building this
  // package configuration.
  //
  if (!cfg_machines.empty ())
  {
    // Calculate the expiration time for package configurations being in the
    // testing state or those, which build failed abnormally.
    //
    timestamp expiration (timestamp::clock::now () -
                          chrono::seconds (options_->build_result_timeout ()));

    uint64_t expiration_ns (
      std::chrono::duration_cast<std::chrono::nanoseconds> (
        expiration.time_since_epoch ()).count ());

    // Prepare the package version prepared query.
    //
    // Note that the number of packages can be large and so, in order not to
    // hold locks for too long, we will restrict the number of packages being
    // queried in a single transaction. To achieve this we will iterate through
    // packages using the OFFSET/LIMIT pair and sort the query result.
    //
    // Note that this approach can result in missing some packages or
    // iterating multiple times over some of them. However there is nothing
    // harmful in that: updates are infrequent and missed packages will be
    // picked up on the next request.
    //
    using pkg_query = query<package_version>;
    using prep_pkg_query = prepared_query<package_version>;

    size_t offset (0); // See the package version query.

    // Skip external and stub packages.
    //
    pkg_query pq ((pkg_query::internal_repository.is_not_null () &&
                   compare_version_ne (pkg_query::id.version,
                                       wildcard_version,
                                       true)) +
                  "ORDER BY" +
                  pkg_query::id.name + "," +
                  pkg_query::id.version.epoch + "," +
                  pkg_query::id.version.canonical_upstream + "," +
                  pkg_query::id.version.canonical_release + "," +
                  pkg_query::id.version.revision +
                  "OFFSET" + pkg_query::_ref (offset) + "LIMIT 50");

    connection_ptr pkg_conn (package_db_->connection ());

    prep_pkg_query pkg_prep_query (
      pkg_conn->prepare_query<package_version> (
        "mod-build-task-package-version-query", pq));

    // Prepare the build prepared query.
    //
    // Note that we can not query the database for configurations that a
    // package was not built with, as the database contains only those package
    // configurations that have already been acted upon (initially empty).
    //
    // This is why we query the database for package configurations that
    // should not be built (in the tested state with the build terminated
    // normally or not expired, or in the testing state and not expired).
    // Having such a list we will select the first build configuration that is
    // not in the list (if available) for the response.
    //
    using bld_query = query<build>;
    using prep_bld_query = prepared_query<build>;

    package_id id; // See the build query.

    const auto& qv (bld_query::id.package.version);

    bld_query bq (
      bld_query::id.package.name == bld_query::_ref (id.name) &&

      qv.epoch == bld_query::_ref (id.version.epoch) &&
      qv.canonical_upstream ==
      bld_query::_ref (id.version.canonical_upstream) &&
      qv.canonical_release == bld_query::_ref (id.version.canonical_release) &&
      qv.revision == bld_query::_ref (id.version.revision) &&

      bld_query::id.configuration.in_range (cfg_names.begin (),
                                            cfg_names.end ()) &&

      ((bld_query::state == "tested" &&
        ((bld_query::status != "abort" && bld_query::status != "abnormal") ||
         bld_query::timestamp > expiration_ns)) ||

       (bld_query::state == "testing" &&
        bld_query::timestamp > expiration_ns)));

    connection_ptr bld_conn (build_db_->connection ());

    prep_bld_query bld_prep_query (
      bld_conn->prepare_query<build> (
        "mod-build-task-package-build-query", bq));

    while (tsm.session.empty ())
    {
      // Start the package database transaction.
      //
      transaction pt (pkg_conn->begin ());

      // Query package versions.
      //
      auto package_versions (pkg_prep_query.execute ());

      // Bail out if there is nothing left.
      //
      if (package_versions.empty ())
      {
        pt.commit ();
        break;
      }

      offset += package_versions.size ();

      // Start the build database transaction.
      //
      {
        transaction bt (bld_conn->begin (), false);
        transaction::current (bt);

        // Iterate over packages until we find one that needs building.
        //
        for (auto& pv: package_versions)
        {
          id = move (pv.id);

          // Iterate through the package configurations and erase those that
          // don't need building from the build configuration map. All those
          // configurations that remained can be built. We will take the first
          // one, if present.
          //
          config_machines configs (cfg_machines); // Make a copy for this pkg.

          for (const auto& pc: bld_prep_query.execute ())
          {
            auto i (configs.find (pc.id.configuration.c_str ()));

            // Outdated configurations are already excluded with the database
            // query.
            //
            assert (i != configs.end ());
            configs.erase (i);
          }

          if (!configs.empty ())
          {
            config_machine& cm (configs.begin ()->second);
            const build_config& cfg (*cm.config);

            build_id bid (move (id), cfg.name);
            shared_ptr<build> b (build_db_->find<build> (bid));

            // If build configuration doesn't exist then create the new one
            // and persist. Otherwise put it into the testing state, refresh
            // the timestamp and update.
            //
            if (b == nullptr)
            {
              b = make_shared<build> (move (bid.package.name),
                                      move (pv.version),
                                      move (bid.configuration));

              build_db_->persist (b);
            }
            else
            {
              // If the package configuration is in the tested state, then we
              // need to cleanup the status and results prior to the update.
              // Otherwise the status is already absent and there are no
              // results.
              //
              // Load the section to make sure results are updated for the
              // tested state, otherwise assert there are no results.
              //
              build_db_->load (*b, b->results_section);

              if (b->state == build_state::tested)
              {
                assert (b->status);
                b->status = nullopt;

                b->results.clear ();
              }
              else
              {
                assert (!b->status && b->results.empty ());
              }

              b->state = build_state::testing;
              b->timestamp = timestamp::clock::now ();

              build_db_->update (b);
            }

            // Finally, prepare the task response manifest.
            //
            tsm.session = b->package_name + '/' +
              b->package_version.string () + '/' + b->configuration;

            // @@ We don't support challenge at the moment, so leave it absent.
            //

            tsm.result_url = options_->host () + options_->root ().string () +
              "?build-result";

            // Switch to the package database transaction to load the package.
            //
            transaction::current (pt);

            shared_ptr<package> p (package_db_->load<package> (b->id.package));
            shared_ptr<repository> r (p->internal_repository.load ());

            // Switch back to the build database transaction.
            //
            transaction::current (bt);

            strings fp;
            if (r->certificate)
              fp.emplace_back (move (r->certificate->fingerprint));

            tsm.task = task_manifest (
              move (b->package_name),
              move (b->package_version),
              move (r->location),
              move (fp),
              move (cm.machine->name),
              cfg.target,
              cfg.vars);
          }

          // If task response manifest is filled, then can bail out from the
          // package loop, commit transactions and respond.
          //
          if (!tsm.session.empty ())
            break;
        }

        bt.commit ();  // Commit the build database transaction.
      }

      transaction::current (pt); // Switch to the package database transaction.
      pt.commit ();
    }
  }

  // @@ Probably it would be a good idea to also send some cache control
  //    headers to avoid caching by HTTP proxies. That would require extension
  //    of the web::response interface.
  //

  manifest_serializer s (rs.content (200, "text/manifest;charset=utf-8"),
                         "task_response_manifest");
  tsm.serialize (s);

  return true;
}
