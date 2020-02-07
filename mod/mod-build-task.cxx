// file      : mod/mod-build-task.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-build-task.hxx>

#include <map>
#include <chrono>

#include <odb/database.hxx>
#include <odb/transaction.hxx>
#include <odb/schema-catalog.hxx>

#include <libbutl/sha256.mxx>
#include <libbutl/utility.mxx>             // compare_c_string
#include <libbutl/openssl.mxx>
#include <libbutl/fdstream.mxx>            // nullfd
#include <libbutl/process-io.mxx>
#include <libbutl/path-pattern.mxx>
#include <libbutl/manifest-parser.mxx>
#include <libbutl/manifest-serializer.mxx>

#include <libbbot/manifest.hxx>
#include <libbbot/build-config.hxx>

#include <web/module.hxx>

#include <libbrep/build.hxx>
#include <libbrep/build-odb.hxx>
#include <libbrep/build-package.hxx>
#include <libbrep/build-package-odb.hxx>

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
      build_config_module (r),
      options_ (r.initialized_ ? r.options_ : nullptr)
{
}

void brep::build_task::
init (scanner& s)
{
  HANDLER_DIAG;

  options_ = make_shared<options::build_task> (
    s, unknown_mode::fail, unknown_mode::fail);

  if (options_->build_config_specified ())
  {
    database_module::init (*options_, options_->build_db_retry ());

    // Check that the database 'build' schema matches the current one. It's
    // enough to perform the check in just a single module implementation
    // (more details in the comment in package_search::init()).
    //
    const string ds ("build");
    if (schema_catalog::current_version (*build_db_, ds) !=
        build_db_->schema_version (ds))
      fail << "database 'build' schema differs from the current one (module "
           << BREP_VERSION_ID << ")";

    build_config_module::init (*options_);
  }

  if (options_->root ().empty ())
    options_->root (dir_path ("/"));
}

bool brep::build_task::
handle (request& rq, response& rs)
{
  HANDLER_DIAG;

  if (build_db_ == nullptr)
    throw invalid_request (501, "not implemented");

  params::build_task params;

  try
  {
    // Note that we expect the task request manifest to be posted and so
    // consider parameters from the URL only.
    //
    name_value_scanner s (rq.parameters (0 /* limit */, true /* url_only */));
    params = params::build_task (s, unknown_mode::fail, unknown_mode::fail);
  }
  catch (const cli::exception& e)
  {
    throw invalid_request (400, e.what ());
  }

  task_request_manifest tqm;

  try
  {
    // We fully cache the request content to be able to retry the request
    // handling if odb::recoverable is thrown (see database-module.cxx for
    // details).
    //
    size_t limit (options_->build_task_request_max_size ());
    manifest_parser p (rq.content (limit, limit), "task_request_manifest");
    tqm = task_request_manifest (p);
  }
  catch (const manifest_parsing& e)
  {
    throw invalid_request (400, e.what ());
  }

  // Obtain the agent's public key fingerprint if requested. If the fingerprint
  // is requested but is not present in the request or is unknown, then respond
  // with 401 HTTP code (unauthorized).
  //
  optional<string> agent_fp;

  if (bot_agent_key_map_ != nullptr)
  {
    if (!tqm.fingerprint ||
        bot_agent_key_map_->find (*tqm.fingerprint) ==
        bot_agent_key_map_->end ())
      throw invalid_request (401, "unauthorized");

    agent_fp = move (tqm.fingerprint);
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
      // The same story as in exclude() from build-config.cxx.
      //
      try
      {
        if (path_match (dash_components_to_path (m.name),
                        dash_components_to_path (c.machine_pattern),
                        dir_path () /* start */,
                        path_match_flags::match_absent) &&
            cfg_machines.insert (
              make_pair (c.name.c_str (), config_machine ({&c, &m}))).second)
          cfg_names.push_back (c.name.c_str ());
      }
      catch (const invalid_path&) {}
    }
  }

  // Go through packages until we find one that has no build configuration
  // present in the database, or is in the building state but expired
  // (collectively called unbuilt). If such a package configuration is found
  // then put it into the building state, set the current timestamp and respond
  // with the task for building this package configuration.
  //
  // While trying to find a non-built package configuration we will also
  // collect the list of the built package configurations which it's time to
  // rebuild. So if no unbuilt package is found, we will pickup one to
  // rebuild. The rebuild preference is given in the following order: the
  // greater force state, the greater overall status, the lower timestamp.
  //
  if (!cfg_machines.empty ())
  {
    vector<shared_ptr<build>> rebuilds;

    // Create the task response manifest. The package must have the internal
    // repository loaded.
    //
    auto task = [this] (shared_ptr<build>&& b,
                        shared_ptr<build_package>&& p,
                        const config_machine& cm) -> task_response_manifest
    {
      uint64_t ts (
        chrono::duration_cast<std::chrono::nanoseconds> (
          b->timestamp.time_since_epoch ()).count ());

      string session (b->tenant + '/' +
                      b->package_name.string () + '/' +
                      b->package_version.string () + '/' +
                      b->configuration + '/' +
                      b->toolchain_name + '/' +
                      b->toolchain_version.string () + '/' +
                      to_string (ts));

      string result_url (options_->host () +
                         tenant_dir (options_->root (), b->tenant).string () +
                         "?build-result");

      lazy_shared_ptr<build_repository> r (p->internal_repository);

      strings fp;
      if (r->certificate_fingerprint)
        fp.emplace_back (move (*r->certificate_fingerprint));

      task_manifest task (move (b->package_name),
                          move (b->package_version),
                          move (r->location),
                          move (fp),
                          cm.machine->name,
                          cm.config->target,
                          cm.config->environment,
                          cm.config->args,
                          cm.config->warning_regexes);

      return task_response_manifest (move (session),
                                     move (b->agent_challenge),
                                     move (result_url),
                                     move (task));
    };

    // Calculate the build (building state) or rebuild (built state) expiration
    // time for package configurations
    //
    timestamp now (system_clock::now ());

    auto expiration = [&now] (size_t timeout) -> timestamp
    {
      return now - chrono::seconds (timeout);
    };

    auto expiration_ns = [&expiration] (size_t timeout) -> uint64_t
    {
      return chrono::duration_cast<chrono::nanoseconds> (
        expiration (timeout).time_since_epoch ()).count ();
    };

    uint64_t normal_result_expiration_ns (
      expiration_ns (options_->build_result_timeout ()));

    uint64_t forced_result_expiration_ns (
      expiration_ns (options_->build_forced_rebuild_timeout ()));

    timestamp normal_rebuild_expiration (
      expiration (options_->build_normal_rebuild_timeout ()));

    timestamp forced_rebuild_expiration (
      expiration (options_->build_forced_rebuild_timeout ()));

    // Return the challenge (nonce) if brep is configured to authenticate bbot
    // agents. Return nullopt otherwise.
    //
    // Nonce generator must guarantee a probabilistically insignificant chance
    // of repeating a previously generated value. The common approach is to use
    // counters or random number generators (alone or in combination), that
    // produce values of the sufficient length. 64-bit non-repeating and
    // 512-bit random numbers are considered to be more than sufficient for
    // most practical purposes.
    //
    // We will produce the challenge as the sha256sum of the 512-bit random
    // number and the 64-bit current timestamp combination. The latter is
    // not really a non-repeating counter and can't be used alone. However
    // adding it is a good and cheap uniqueness improvement.
    //
    auto challenge = [&agent_fp, &now, &fail, &trace, this] ()
    {
      optional<string> r;

      if (agent_fp)
      {
        try
        {
          auto print_args = [&trace, this] (const char* args[], size_t n)
          {
            l2 ([&]{trace << process_args {args, n};});
          };

          openssl os (print_args,
                      nullfd, path ("-"), 2,
                      process_env (options_->openssl (),
                                   options_->openssl_envvar ()),
                      "rand",
                      options_->openssl_option (), 64);

          vector<char> nonce (os.in.read_binary ());
          os.in.close ();

          if (!os.wait () || nonce.size () != 64)
            fail << "unable to generate nonce";

          uint64_t t (chrono::duration_cast<std::chrono::nanoseconds> (
                        now.time_since_epoch ()).count ());

          sha256 cs (nonce.data (), nonce.size ());
          cs.append (&t, sizeof (t));
          r = cs.string ();
        }
        catch (const system_error& e)
        {
          fail << "unable to generate nonce: " << e;
        }
      }

      return r;
    };

    // Convert butl::standard_version type to brep::version.
    //
    brep::version toolchain_version (tqm.toolchain_version.string ());

    // Prepare the buildable package prepared query.
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
    // Also note that we disregard the request tenant and operate on the whole
    // set of the packages and builds. In future we may add support for
    // building packages for a specific tenant.
    //
    using pkg_query = query<buildable_package>;
    using prep_pkg_query = prepared_query<buildable_package>;

    // Exclude archived tenants.
    //
    pkg_query pq (!pkg_query::build_tenant::archived);

    // Filter by repositories canonical names (if requested).
    //
    const vector<string>& rp (params.repository ());

    if (!rp.empty ())
      pq = pq &&
        pkg_query::build_repository::id.canonical_name.in_range (rp.begin (),
                                                                 rp.end ());

    // Specify the portion.
    //
    size_t offset (0);

    pq += "ORDER BY" +
      pkg_query::build_package::id.tenant + "," +
      pkg_query::build_package::id.name +
      order_by_version (pkg_query::build_package::id.version, false) +
      "OFFSET" + pkg_query::_ref (offset) + "LIMIT 50";

    connection_ptr conn (build_db_->connection ());

    prep_pkg_query pkg_prep_query (
      conn->prepare_query<buildable_package> (
        "mod-build-task-package-query", pq));

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

    package_id id;
    const auto& qv (bld_query::id.package.version);

    bld_query bq (
      bld_query::id.package.tenant == bld_query::_ref (id.tenant) &&

      bld_query::id.package.name == bld_query::_ref (id.name)     &&

      qv.epoch == bld_query::_ref (id.version.epoch)              &&
      qv.canonical_upstream ==
        bld_query::_ref (id.version.canonical_upstream)           &&
      qv.canonical_release ==
        bld_query::_ref (id.version.canonical_release) &&
      qv.revision == bld_query::_ref (id.version.revision)        &&

      bld_query::id.configuration.in_range (cfg_names.begin (),
                                            cfg_names.end ())     &&

      bld_query::id.toolchain_name == tqm.toolchain_name          &&

      compare_version_eq (bld_query::id.toolchain_version,
                          canonical_version (toolchain_version),
                          true /* revision */)                    &&

      (bld_query::state == "built" ||
       ((bld_query::force == "forcing" &&
         bld_query::timestamp > forced_result_expiration_ns) ||
        (bld_query::force != "forcing" && // Unforced or forced.
         bld_query::timestamp > normal_result_expiration_ns))));

    prep_bld_query bld_prep_query (
      conn->prepare_query<build> ("mod-build-task-build-query", bq));

    while (tsm.session.empty ())
    {
      transaction t (conn->begin ());

      // Query (and cache) buildable packages.
      //
      auto packages (pkg_prep_query.execute ());

      // Bail out if there is nothing left.
      //
      if (packages.empty ())
      {
        t.commit ();
        break;
      }

      offset += packages.size ();

      // Iterate over packages until we find one that needs building.
      //
      for (auto& bp: packages)
      {
        id = move (bp.id);

        // Iterate through the package configurations and erase those that
        // don't need building from the build configuration map. All those
        // configurations that remained can be built. We will take the first
        // one, if present.
        //
        // Also save the built package configurations for which it's time to be
        // rebuilt.
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

          if (i->state == build_state::built)
          {
            assert (i->force != force_state::forcing);

            if (i->timestamp <= (i->force == force_state::forced
                                 ? forced_rebuild_expiration
                                 : normal_rebuild_expiration))
              rebuilds.emplace_back (i.load ());
          }
        }

        if (!configs.empty ())
        {
          // Find the first build configuration that is not excluded by the
          // package.
          //
          shared_ptr<build_package> p (build_db_->load<build_package> (id));

          auto i (configs.begin ());
          auto e (configs.end ());

          for (;
               i != e &&
               exclude (p->builds, p->constraints, *i->second.config);
               ++i) ;

          if (i != e)
          {
            config_machine& cm (i->second);
            machine_header_manifest& mh (*cm.machine);

            build_id bid (move (id),
                          cm.config->name,
                          move (tqm.toolchain_name),
                          toolchain_version);

            shared_ptr<build> b (build_db_->find<build> (bid));
            optional<string> cl (challenge ());

            // If build configuration doesn't exist then create the new one
            // and persist. Otherwise put it into the building state, refresh
            // the timestamp and update.
            //
            if (b == nullptr)
            {
              b = make_shared<build> (move (bid.package.tenant),
                                      move (bid.package.name),
                                      move (bp.version),
                                      move (bid.configuration),
                                      move (bid.toolchain_name),
                                      move (toolchain_version),
                                      move (agent_fp),
                                      move (cl),
                                      mh.name,
                                      move (mh.summary),
                                      cm.config->target);

              build_db_->persist (b);
            }
            else
            {
              // The package configuration is in the building state, and there
              // are no results.
              //
              // Note that in both cases we keep the status intact to be able
              // to compare it with the final one in the result request
              // handling in order to decide if to send the notification
              // email. The same is true for the forced flag (in the sense
              // that we don't set the force state to unforced).
              //
              // Load the section to assert the above statement.
              //
              build_db_->load (*b, b->results_section);

              assert (b->state == build_state::building &&
                      b->results.empty ());

              b->state = build_state::building;

              // Switch the force state not to reissue the task after the
              // forced rebuild timeout. Note that the result handler will
              // still recognize that the rebuild was forced.
              //
              if (b->force == force_state::forcing)
                b->force = force_state::forced;

              b->agent_fingerprint = move (agent_fp);
              b->agent_challenge = move (cl);
              b->machine = mh.name;
              b->machine_summary = move (mh.summary);
              b->target = cm.config->target;
              b->timestamp = system_clock::now ();

              build_db_->update (b);
            }

            // Finally, prepare the task response manifest.
            //
            // We iterate over buildable packages.
            //
            assert (p->internal_repository != nullptr);

            p->internal_repository.load ();

            tsm = task (move (b), move (p), cm);
          }
        }

        // If the task response manifest is prepared, then bail out from the
        // package loop, commit the transaction and respond.
        //
        if (!tsm.session.empty ())
          break;
      }

      t.commit ();
    }

    // If we don't have an unbuilt package, then let's see if we have a
    // package to rebuild.
    //
    if (tsm.session.empty () && !rebuilds.empty ())
    {
      // Sort the package configuration rebuild list with the following sort
      // priority:
      //
      // 1: force state
      // 2: overall status
      // 3: timestamp (less is preferred)
      //
      auto cmp = [] (const shared_ptr<build>& x, const shared_ptr<build>& y)
      {
        if (x->force != y->force)
          return x->force > y->force;       // Forced goes first.

        assert (x->status && y->status);    // Both built.

        if (x->status != y->status)
          return x->status > y->status;     // Larger status goes first.

        return x->timestamp < y->timestamp; // Older goes first.
      };

      sort (rebuilds.begin (), rebuilds.end (), cmp);

      optional<string> cl (challenge ());

      // Pick the first package configuration from the ordered list.
      //
      // Note that the configurations and packages may not match the required
      // criteria anymore (as we have committed the database transactions that
      // were used to collect this data) so we recheck. If we find one that
      // matches then put it into the building state, refresh the timestamp and
      // update. Note that we don't amend the status and the force state to
      // have them available in the result request handling (see above).
      //
      for (auto& b: rebuilds)
      {
        try
        {
          transaction t (build_db_->begin ());

          b = build_db_->find<build> (b->id);

          if (b != nullptr && b->state == build_state::built &&
              b->timestamp <= (b->force == force_state::forced
                               ? forced_rebuild_expiration
                               : normal_rebuild_expiration))
          {
            auto i (cfg_machines.find (b->id.configuration.c_str ()));

            // Only actual package configurations are loaded (see above).
            //
            assert (i != cfg_machines.end ());
            const config_machine& cm (i->second);

            // Rebuild the package if still present, is buildable and doesn't
            // exclude the configuration.
            //
            shared_ptr<build_package> p (
              build_db_->find<build_package> (b->id.package));

            if (p != nullptr                      &&
                p->internal_repository != nullptr &&
                !exclude (p->builds, p->constraints, *cm.config))
            {
              assert (b->status);

              b->state = build_state::building;

              // Can't move from, as may need them on the next iteration.
              //
              b->agent_fingerprint = agent_fp;
              b->agent_challenge = cl;

              const machine_header_manifest& mh (*cm.machine);
              b->machine = mh.name;
              b->machine_summary = mh.summary;

              b->target = cm.config->target;

              // Mark the section as loaded, so results are updated.
              //
              b->results_section.load ();
              b->results.clear ();

              b->timestamp = system_clock::now ();

              build_db_->update (b);

              p->internal_repository.load ();

              tsm = task (move (b), move (p), cm);
            }
          }

          t.commit ();
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
