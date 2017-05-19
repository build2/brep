// file      : mod/mod-build-task.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-build-task.hxx>

#include <map>
#include <chrono>

#include <odb/database.hxx>
#include <odb/transaction.hxx>
#include <odb/schema-catalog.hxx>

#include <libbutl/utility.hxx>             // compare_c_string
#include <libbutl/filesystem.hxx>          // path_match()
#include <libbutl/manifest-parser.hxx>
#include <libbutl/manifest-serializer.hxx>

#include <libbbot/manifest.hxx>
#include <libbbot/build-config.hxx>

#include <web/module.hxx>

#include <libbrep/build.hxx>
#include <libbrep/build-odb.hxx>
#include <libbrep/package.hxx>
#include <libbrep/package-odb.hxx>

#include <mod/options.hxx>

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
  {
    database_module::init (static_cast<options::build>    (*options_),
                           static_cast<options::build_db> (*options_),
                           options_->build_db_retry ());

    // Check that the database 'build' schema matches the current one. It's
    // enough to perform the check in just a single module implementation
    // (more details in the comment in package_search::init()).
    //
    const string ds ("build");
    if (schema_catalog::current_version (*build_db_, ds) !=
        build_db_->schema_version (ds))
      fail << "database 'build' schema differs from the current one (module "
           << BREP_VERSION_ID << ")";
  }

  if (options_->root ().empty ())
    options_->root (dir_path ("/"));
}

bool brep::build_task::
handle (request& rq, response& rs)
{
  MODULE_DIAG;

  if (build_db_ == nullptr)
    throw invalid_request (501, "not implemented");

  params::build_task params;

  try
  {
    name_value_scanner s (rq.parameters ());
    params = params::build_task (s, unknown_mode::fail, unknown_mode::fail);
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
    machine_header_manifest* machine;
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
  // present in the database, or has the unbuilt one, or in the building state
  // but expired (collectively called unbuilt). If such a package
  // configuration is found then put it into the building state, set the
  // current timestamp and respond with the task for building this package
  // configuration.
  //
  // While trying to find a non-built package configuration we will also
  // collect the list of the built package configurations which it's time to
  // rebuilt. So if no unbuilt package is found, we will pickup one to
  // rebuild. The rebuild preference is given in the following order: the
  // greater force flag, the greater overall status, the lower timestamp.
  //
  if (!cfg_machines.empty ())
  {
    vector<shared_ptr<build>> rebuilds;

    // Create the task response manifest. The package must have the internal
    // repository loaded.
    //
    auto task = [this] (shared_ptr<build>&& b,
                        shared_ptr<package>&& p,
                        const config_machine& cm) -> task_response_manifest
    {
      uint64_t ts (
        chrono::duration_cast<std::chrono::nanoseconds> (
          b->timestamp.time_since_epoch ()).count ());

      string session (b->package_name + '/' + b->package_version.string () +
                      '/' + b->configuration +
                      '/' + b->toolchain_version.string () +
                      '/' + to_string (ts));

      string result_url (options_->host () + options_->root ().string () +
                         "?build-result");

      lazy_shared_ptr<repository> r (p->internal_repository);

      strings fp;
      if (r->certificate)
        fp.emplace_back (move (r->certificate->fingerprint));

      task_manifest task (move (b->package_name),
                          move (b->package_version),
                          move (r->location),
                          move (fp),
                          cm.machine->name,
                          cm.config->target,
                          cm.config->vars);

      // @@ We don't support challenge at the moment.
      //
      return task_response_manifest (move (session),
                                     nullopt,
                                     move (result_url),
                                     move (task));
    };

    // Calculate the expiration time for package configurations being in the
    // building (build expiration) or the built (rebuild expiration) state.
    //
    timestamp now (timestamp::clock::now ());

    auto expiration = [&now] (size_t timeout) -> timestamp
    {
      return now - chrono::seconds (timeout);
    };

    auto expiration_ns = [&expiration] (size_t timeout) -> uint64_t
    {
      return chrono::duration_cast<chrono::nanoseconds> (
        expiration (timeout).time_since_epoch ()).count ();
    };

    uint64_t build_expiration_ns (
      expiration_ns (options_->build_result_timeout ()));

    timestamp forced_rebuild_expiration (
      expiration (options_->build_forced_rebuild_timeout ()));

    timestamp normal_rebuild_expiration (
      expiration (options_->build_normal_rebuild_timeout ()));

    // Convert butl::standard_version type to brep::version.
    //
    brep::version toolchain_version (tqm.toolchain_version.string ());

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
    pkg_query pq (pkg_query::package::internal_repository.is_not_null () &&
                  compare_version_ne (pkg_query::package::id.version,
                                      wildcard_version,
                                      false));

    // Filter by repositories display names (if requested).
    //
    const vector<string>& rp (params.repository ());

    if (!rp.empty ())
      pq = pq &&
        pkg_query::repository::display_name.in_range (rp.begin (), rp.end ());

    // Specify the portion.
    //
    pq += "ORDER BY" +
      pkg_query::package::id.name + "," +
      pkg_query::package::id.version.epoch + "," +
      pkg_query::package::id.version.canonical_upstream + "," +
      pkg_query::package::id.version.canonical_release + "," +
      pkg_query::package::id.version.revision +
      "OFFSET" + pkg_query::_ref (offset) + "LIMIT 50";

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
    // should not be built (in the built state, or in the building state and
    // not expired). Having such a list we will select the first build
    // configuration that is not in the list (if available) for the response.
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

      compare_version_eq (bld_query::id.toolchain_version,
                          toolchain_version,
                          true) &&

      (bld_query::state == "built" ||
       (bld_query::state == "building" &&
        bld_query::timestamp > build_expiration_ns)));

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
          // Also save the built package configurations for which it's time
          // to be rebuilt.
          //
          config_machines configs (cfg_machines); // Make a copy for this pkg.
          auto pkg_builds (bld_prep_query.execute ());

          for (auto i (pkg_builds.begin ()); i != pkg_builds.end (); ++i)
          {
            auto j (configs.find (i->id.configuration.c_str ()));

            // Outdated configurations are already excluded with the database
            // query.
            //
            assert (j != configs.end ());
            configs.erase (j);

            if (i->state == build_state::built &&
                i->timestamp <= (i->forced
                                 ? forced_rebuild_expiration
                                 : normal_rebuild_expiration))
              rebuilds.emplace_back (i.load ());
          }

          if (!configs.empty ())
          {
            config_machine& cm (configs.begin ()->second);
            machine_header_manifest& mh (*cm.machine);
            build_id bid (move (id), cm.config->name, toolchain_version);
            shared_ptr<build> b (build_db_->find<build> (bid));

            // If build configuration doesn't exist then create the new one
            // and persist. Otherwise put it into the building state, refresh
            // the timestamp and update.
            //
            if (b == nullptr)
            {
              b = make_shared<build> (move (bid.package.name),
                                      move (pv.version),
                                      move (bid.configuration),
                                      move (tqm.toolchain_name),
                                      move (toolchain_version),
                                      mh.name,
                                      move (mh.summary),
                                      cm.config->target);

              build_db_->persist (b);
            }
            else
            {
              // The package configuration can be in the building or unbuilt
              // state, so the forced flag is false and the status is absent,
              // unless in the building state (in which case they may or may
              // not be set/exist), and there are no results.
              //
              // Note that in the building state the status can be present if
              // the rebuild task have been issued. In both cases we keep the
              // status intact to be able to compare it with the final one in
              // the result request handling in order to decide if to send the
              // notification email. The same is true for the forced flag. We
              // just assert that if the force flag is set, then the status
              // exists.
              //
              // Load the section to assert the above statement.
              //
              build_db_->load (*b, b->results_section);

              assert (b->state != build_state::built &&

                      ((!b->forced && !b->status) ||
                       (b->state == build_state::building &&
                        (!b->forced || b->status))) &&

                      b->results.empty ());

              b->state = build_state::building;
              b->toolchain_name = move (tqm.toolchain_name);
              b->machine = mh.name;
              b->machine_summary = move (mh.summary);
              b->target = cm.config->target;
              b->timestamp = timestamp::clock::now ();

              build_db_->update (b);
            }

            // Finally, prepare the task response manifest.
            //
            // Switch to the package database transaction to load the package.
            //
            transaction::current (pt);

            shared_ptr<package> p (package_db_->load<package> (b->id.package));
            p->internal_repository.load ();

            // Switch back to the build database transaction.
            //
            transaction::current (bt);

            tsm = task (move (b), move (p), cm);
          }

          // If the task response manifest is prepared, then bail out from the
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

    // If we don't have an unbuilt package, then let's see if we have a
    // package to rebuild.
    //
    if (tsm.session.empty () && !rebuilds.empty ())
    {
      // Sort the package configuration rebuild list with the following sort
      // priority:
      //
      // 1: forced flag
      // 2: overall status
      // 3: timestamp (less is preferred)
      //
      auto cmp = [] (const shared_ptr<build>& x,
                     const shared_ptr<build>& y) -> bool
      {
        if (x->forced != y->forced)
          return x->forced > y->forced;     // Forced goes first.

        assert (x->status && y->status);    // Both built.

        if (x->status != y->status)
          return x->status > y->status;     // Larger status goes first.

        return x->timestamp < y->timestamp; // Older goes first.
      };

      sort (rebuilds.begin (), rebuilds.end (), cmp);

      // Pick the first package configuration from the ordered list.
      //
      // Note that the configurations may not match the required criteria
      // anymore (as we have committed the database transactions that were
      // used to collect this data) so we recheck. If we find one that matches
      // then put it into the building state, refresh the timestamp and
      // update. Note that we don't amend the status and the force flag to
      // have them available in the result request handling (see above).
      //
      for (auto& b: rebuilds)
      {
        try
        {
          transaction bt (build_db_->begin ());

          b = build_db_->find<build> (b->id);

          if (b != nullptr && b->state == build_state::built &&
              b->timestamp <= (b->forced
                               ? forced_rebuild_expiration
                               : normal_rebuild_expiration))
          {
            auto i (cfg_machines.find (b->id.configuration.c_str ()));

            // Only actual package configurations are loaded (see above).
            //
            assert (i != cfg_machines.end ());
            const config_machine& cm (i->second);
            const machine_header_manifest& mh (*cm.machine);

            // Load the package (if still present).
            //
            transaction pt (package_db_->begin (), false);
            transaction::current (pt);

            shared_ptr<package> p (package_db_->find<package> (b->id.package));

            if (p != nullptr)
              p->internal_repository.load ();

            // Commit the package database transaction and switch back to the
            // build database transaction.
            //
            pt.commit ();
            transaction::current (bt);

            if (p != nullptr)
            {
              assert (b->status);

              b->state = build_state::building;
              b->machine = mh.name;

              // Can't move from, as may need it on the next iteration.
              //
              b->toolchain_name = tqm.toolchain_name;
              b->machine_summary = mh.summary;

              b->target = cm.config->target;

              // Mark the section as loaded, so results are updated.
              //
              b->results_section.load ();
              b->results.clear ();

              b->timestamp = timestamp::clock::now ();

              build_db_->update (b);

              tsm = task (move (b), move (p), cm);
            }
          }

          bt.commit ();
        }
        catch (const odb::deadlock&) {} // Just try with the next rebuild.

        // If the task response manifest is prepared, then bail out from the
        // package configuration rebuilds loop and respond.
        //
        if (!tsm.session.empty ())
          break;
      }
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
