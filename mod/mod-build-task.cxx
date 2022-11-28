// file      : mod/mod-build-task.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-build-task.hxx>

#include <map>
#include <regex>
#include <chrono>

#include <odb/database.hxx>
#include <odb/transaction.hxx>
#include <odb/schema-catalog.hxx>

#include <libbutl/regex.hxx>
#include <libbutl/sha256.hxx>
#include <libbutl/openssl.hxx>
#include <libbutl/fdstream.hxx>            // nullfd
#include <libbutl/process-io.hxx>
#include <libbutl/path-pattern.hxx>
#include <libbutl/manifest-parser.hxx>
#include <libbutl/manifest-serializer.hxx>

#include <libbbot/manifest.hxx>

#include <web/server/module.hxx>

#include <libbrep/build.hxx>
#include <libbrep/build-odb.hxx>
#include <libbrep/build-package.hxx>
#include <libbrep/build-package-odb.hxx>

#include <mod/build-target-config.hxx>

#include <mod/module-options.hxx>

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
    // Verify that build-alt-*-rebuild-{start,stop} are both either specified
    // or not.
    //
    auto bad_alt = [&fail] (const char* what)
    {
      fail << "build-alt-" << what << "-rebuild-start and build-alt-" << what
           << "-rebuild-stop configuration options must both be either "
           << "specified or not";
    };

    if (options_->build_alt_soft_rebuild_start_specified () !=
        options_->build_alt_soft_rebuild_stop_specified ())
      bad_alt ("soft");

    if (options_->build_alt_hard_rebuild_start_specified () !=
        options_->build_alt_hard_rebuild_stop_specified ())
      bad_alt ("hard");

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

  // Map build target configurations to machines that are capable of building
  // them. The first matching machine is selected for each configuration.
  //
  struct config_machine
  {
    const build_target_config* config;
    machine_header_manifest* machine;
  };

  using config_machines = map<build_target_config_id, config_machine>;

  config_machines conf_machines;

  for (const auto& c: *target_conf_)
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
                        path_match_flags::match_absent))
          conf_machines.emplace (build_target_config_id {c.target, c.name},
                                 config_machine {&c, &m});
      }
      catch (const invalid_path&) {}
    }
  }

  // Go through package build configurations until we find one that has no
  // build target configuration present in the database, or is in the building
  // state but expired (collectively called unbuilt). If such a target
  // configuration is found then put it into the building state, set the
  // current timestamp and respond with the task for building this package
  // configuration.
  //
  // While trying to find a non-built package configuration we will also
  // collect the list of the built configurations which it's time to
  // rebuild. So if no unbuilt package configuration is found, we will pickup
  // one to rebuild. The rebuild preference is given in the following order:
  // the greater force state, the greater overall status, the lower timestamp.
  //
  if (!conf_machines.empty ())
  {
    vector<shared_ptr<build>> rebuilds;

    // Create the task response manifest. Must be called inside the build db
    // transaction.
    //
    auto task = [this] (shared_ptr<build>&& b,
                        shared_ptr<build_package>&& p,
                        build_package_config&& pc,
                        shared_ptr<build_tenant>&& t,
                        const config_machine& cm) -> task_response_manifest
    {
      uint64_t ts (
        chrono::duration_cast<std::chrono::nanoseconds> (
          b->timestamp.time_since_epoch ()).count ());

      string session (b->tenant + '/'                      +
                      b->package_name.string () + '/'      +
                      b->package_version.string () + '/'   +
                      b->target.string () + '/'            +
                      b->target_config_name + '/'          +
                      b->package_config_name + '/'         +
                      b->toolchain_name + '/'              +
                      b->toolchain_version.string () + '/' +
                      to_string (ts));

      string result_url (options_->host () +
                         tenant_dir (options_->root (), b->tenant).string () +
                         "?build-result");

      assert (transaction::has_current ());

      assert (p->internal ()); // The package is expected to be buildable.

      lazy_shared_ptr<build_repository> r (p->internal_repository.load ());

      strings fps;
      if (r->certificate_fingerprint)
        fps.emplace_back (move (*r->certificate_fingerprint));

      // Exclude external test packages which exclude the task build
      // configuration.
      //
      small_vector<bpkg::test_dependency, 1> tests;

      for (const build_test_dependency& td: p->tests)
      {
        // Don't exclude unresolved external tests.
        //
        // Note that this may result in the build task failure. However,
        // silently excluding such tests could end up with missed software
        // bugs which feels much worse.
        //
        if (td.package != nullptr)
        {
          shared_ptr<build_package> p (td.package.load ());

          // Use the default test package configuration.
          //
          // Note that potentially the test package default configuration may
          // contain some (bpkg) arguments associated, but we currently don't
          // provide build bot worker with such information. This, however, is
          // probably too far fetched so let's keep it simple for now.
          //
          const build_package_config* pc (find ("default", p->configs));
          assert (pc != nullptr); // Must always be present.

          // Use the `all` class as a least restrictive default underlying
          // build class set. Note that we should only apply the explicit
          // build restrictions to the external test packages (think about
          // the `builds: all` and `builds: -windows` manifest values for
          // the primary and external test packages, respectively).
          //
          if (exclude (*pc,
                       p->builds,
                       p->constraints,
                       *cm.config,
                       nullptr /* reason */,
                       true /* default_all_ucs */))
            continue;
        }

        tests.emplace_back (move (td.name),
                            td.type,
                            td.buildtime,
                            move (td.constraint),
                            move (td.reflect));
      }

      bool module_pkg (
        b->package_name.string ().compare (0, 10, "libbuild2-") == 0);

      task_manifest task (move (b->package_name),
                          move (b->package_version),
                          move (r->location),
                          move (fps),
                          move (p->requirements),
                          move (tests),
                          move (b->dependency_checksum),
                          cm.machine->name,
                          cm.config->target,
                          cm.config->environment,
                          cm.config->args,
                          move (pc.arguments),
                          belongs (*cm.config, module_pkg ? "build2" : "host"),
                          cm.config->warning_regexes,
                          move (t->interactive),
                          move (b->worker_checksum));

      return task_response_manifest (move (session),
                                     move (b->agent_challenge),
                                     move (result_url),
                                     move (b->agent_checksum),
                                     move (task));
    };

    // Calculate the build (building state) or rebuild (built state)
    // expiration time for package configurations.
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

    timestamp forced_rebuild_expiration (
      expiration (options_->build_forced_rebuild_timeout ()));

    // Calculate the soft/hard rebuild expiration time, based on the
    // respective build-{soft,hard}-rebuild-timeout and
    // build-alt-{soft,hard}-rebuild-{start,stop,timeout} configuration
    // options.
    //
    // If normal_timeout is zero, then return timestamp_unknown to indicate
    // 'never expire'. Note that this value is less than any build timestamp
    // value, including timestamp_nonexistent.
    //
    // NOTE: there is a similar code in monitor/monitor.cxx.
    //
    auto build_expiration = [&now] (
      const optional<pair<duration, duration>>& alt_interval,
      optional<size_t> alt_timeout,
      size_t normal_timeout)
    {
      if (normal_timeout == 0)
        return timestamp_unknown;

      timestamp r;
      chrono::seconds nt (normal_timeout);

      if (alt_interval)
      {
        const duration& start (alt_interval->first);
        const duration& stop  (alt_interval->second);

        duration dt (daytime (now));

        // Note that if the stop time is less than the start time then the
        // interval extends through the midnight.
        //
        bool use_alt_timeout (start <= stop
                              ? dt >= start && dt < stop
                              : dt >= start || dt < stop);

        // If we out of the alternative rebuild timeout interval, then fall
        // back to using the normal rebuild timeout.
        //
        if (use_alt_timeout)
        {
          // Calculate the alternative timeout, unless it is specified
          // explicitly.
          //
          duration t;

          if (!alt_timeout)
          {
            t = start <= stop ? (stop - start) : ((24h - start) + stop);

            // If the normal rebuild timeout is greater than 24 hours, then
            // increase the default alternative timeout by (normal - 24h) (see
            // build-alt-soft-rebuild-timeout configuration option for
            // details).
            //
            if (nt > 24h)
              t += nt - 24h;
          }
          else
            t = chrono::seconds (*alt_timeout);

          r = now - t;
        }
      }

      return r != timestamp_nonexistent ? r : (now - nt);
    };

    timestamp soft_rebuild_expiration (
      build_expiration (
        (options_->build_alt_soft_rebuild_start_specified ()
         ? make_pair (options_->build_alt_soft_rebuild_start (),
                      options_->build_alt_soft_rebuild_stop ())
         : optional<pair<duration, duration>> ()),
        (options_->build_alt_soft_rebuild_timeout_specified ()
         ? options_->build_alt_soft_rebuild_timeout ()
         : optional<size_t> ()),
        options_->build_soft_rebuild_timeout ()));

    timestamp hard_rebuild_expiration (
      build_expiration (
        (options_->build_alt_hard_rebuild_start_specified ()
         ? make_pair (options_->build_alt_hard_rebuild_start (),
                      options_->build_alt_hard_rebuild_stop ())
         : optional<pair<duration, duration>> ()),
        (options_->build_alt_hard_rebuild_timeout_specified ()
         ? options_->build_alt_hard_rebuild_timeout ()
         : optional<size_t> ()),
        options_->build_hard_rebuild_timeout ()));

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

          uint64_t t (chrono::duration_cast<chrono::nanoseconds> (
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

    // Transform (in-place) the interactive login information into the actual
    // login command, if specified in the manifest and the transformation
    // regexes are specified in the configuration.
    //
    if (tqm.interactive_login &&
        options_->build_interactive_login_specified ())
    {
      optional<string> lc;
      string l (tqm.agent + ' ' + *tqm.interactive_login);

      // Use the first matching regex for the transformation.
      //
      for (const pair<regex, string>& rf: options_->build_interactive_login ())
      {
        pair<string, bool> r (regex_replace_match (l, rf.first, rf.second));

        if (r.second)
        {
          lc = move (r.first);
          break;
        }
      }

      if (!lc)
        throw invalid_request (400, "unable to match login info '" + l + '\'');

      tqm.interactive_login = move (lc);
    }

    // If the interactive mode if false or true, then filter out the
    // respective packages. Otherwise, order them so that packages from the
    // interactive build tenants appear first.
    //
    interactive_mode imode (tqm.effective_interactive_mode ());

    switch (imode)
    {
    case interactive_mode::false_:
      {
        pq = pq && pkg_query::build_tenant::interactive.is_null ();
        break;
      }
    case interactive_mode::true_:
      {
        pq = pq && pkg_query::build_tenant::interactive.is_not_null ();
        break;
      }
    case interactive_mode::both: break; // See below.
    }

    // Specify the portion.
    //
    size_t offset (0);

    pq += "ORDER BY";

    if (imode == interactive_mode::both)
      pq += pkg_query::build_tenant::interactive + "NULLS LAST,";

    pq += pkg_query::build_package::id.tenant + "," +
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
    // package was not built with, as the database contains only those build
    // configurations that have already been acted upon (initially empty).
    //
    // This is why we query the database for configurations that should not be
    // built (in the built state, or in the building state and not expired).
    // Having such a list we will select the first build configuration that is
    // not in the list (if available) for the response.
    //
    using bld_query = query<build>;
    using prep_bld_query = prepared_query<build>;

    package_id id;
    string pkg_config_name;

    bld_query sq (false);
    for (const auto& cm: conf_machines)
      sq = sq || (bld_query::id.target == cm.first.target             &&
                  bld_query::id.target_config_name == cm.first.config &&
                  bld_query::id.package_config_name ==
                    bld_query::_ref (pkg_config_name));

    bld_query bq (
      equal<build> (bld_query::id.package, id)                   &&
      sq                                                         &&
      bld_query::id.toolchain_name == tqm.toolchain_name         &&

      compare_version_eq (bld_query::id.toolchain_version,
                          canonical_version (toolchain_version),
                          true /* revision */)                   &&

      (bld_query::state == "built"                          ||
       (bld_query::force == "forcing" &&
        bld_query::timestamp > forced_result_expiration_ns) ||
       (bld_query::force != "forcing" && // Unforced or forced.
        bld_query::timestamp > normal_result_expiration_ns)));

    prep_bld_query bld_prep_query (
      conn->prepare_query<build> ("mod-build-task-build-query", bq));

    // Return true if a package needs to be rebuilt.
    //
    auto needs_rebuild = [&forced_rebuild_expiration,
                          &soft_rebuild_expiration,
                          &hard_rebuild_expiration] (const build& b)
    {
      assert (b.state == build_state::built);

      return (b.force == force_state::forced &&
              b.timestamp <= forced_rebuild_expiration)  ||
             b.soft_timestamp <= soft_rebuild_expiration ||
             b.hard_timestamp <= hard_rebuild_expiration;
    };

    // Convert a build to the hard rebuild, resetting the agent checksum and
    // dropping the previous build task result.
    //
    // Note that since the checksums are hierarchical, the agent checksum
    // reset will trigger resets of the "subordinate" checksums up to the
    // dependency checksum and so the package will be rebuilt.
    //
    // Also note that there is no sense to keep the build task result since we
    // don't accept the skip result for the hard rebuild task. We, however,
    // keep the status intact (see below for the reasoning).
    //
    auto convert_to_hard = [] (const shared_ptr<build>& b)
    {
      b->agent_checksum = nullopt;

      // Mark the section as loaded, so results are updated.
      //
      b->results_section.load ();
      b->results.clear ();
    };

    // Return SHA256 checksum of the controller logic and the configuration
    // target, environment, arguments, and warning-detecting regular
    // expressions.
    //
    auto controller_checksum = [] (const build_target_config& c)
    {
      sha256 cs ("1"); // Hash the logic version.

      cs.append (c.target.string ());
      cs.append (c.environment ? *c.environment : "");

      for (const string& a: c.args)
        cs.append (a);

      for (const string& re: c.warning_regexes)
        cs.append (re);

      return string (cs.string ());
    };

    // Return the machine id as a machine checksum.
    //
    auto machine_checksum = [] (const machine_header_manifest& m)
    {
      return m.id;
    };

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

        shared_ptr<build_package> p (build_db_->load<build_package> (id));

        // Note that a request to interactively build a package in multiple
        // configurations is most likely a mistake than a deliberate choice.
        // Thus, for the interactive tenant let's check if the package can be
        // built in multiple configurations. If that's the case then we will
        // put all the potential builds into the aborted state and continue
        // iterating looking for another package. Otherwise, just proceed for
        // this package normally.
        //
        // It also feels like a good idea to archive an interactive tenant
        // after a build object is created for it, regardless if the build
        // task is issued or not. This way we make sure that an interactive
        // build is never performed multiple times for such a tenant for any
        // reason (multiple toolchains, buildtab change, etc). Note that the
        // build result will still be accepted for an archived build.
        //
        shared_ptr<build_tenant> t (build_db_->load<build_tenant> (id.tenant));

        if (t->interactive)
        {
          // Note that the tenant can be archived via some other package on
          // some previous iteration. Skip the package if that's the case.
          //
          if (t->archived)
            continue;

          // Collect the potential build configurations as all combinations of
          // the tenant's packages build configurations and the non-excluded
          // (by the packages) build target configurations. Note that here we
          // ignore the machines from the task request.
          //
          struct build_config
          {
            package_id                 pid;
            string                     pc;
            const build_target_config* tc;
          };

          small_vector<build_config, 1> build_configs;

          // Note that we don't bother creating a prepared query here, since
          // its highly unlikely to encounter multiple interactive tenants per
          // task request. Given that we archive such tenants immediately, as
          // a common case there will be none.
          //
          pkg_query pq (pkg_query::build_tenant::id == t->id);
          for (auto& tp: build_db_->query<buildable_package> (pq))
          {
            shared_ptr<build_package> p (
              build_db_->load<build_package> (tp.id));

            for (build_package_config& pc: p->configs)
            {
              for (const auto& tc: *target_conf_)
              {
                if (!exclude (pc, p->builds, p->constraints, tc))
                  build_configs.push_back (build_config {p->id, pc.name, &tc});
              }
            }
          }

          // If multiple build configurations are collected, then abort all
          // the potential builds and continue iterating over the packages.
          //
          if (build_configs.size () > 1)
          {
            // Abort the builds.
            //
            for (build_config& c: build_configs)
            {
              const string& pc (c.pc);
              const build_target_config& tc (*c.tc);

              build_id bid (c.pid,
                            tc.target,
                            tc.name,
                            pc,
                            tqm.toolchain_name,
                            toolchain_version);

              // Can there be any existing builds for such a tenant? Doesn't
              // seem so, unless due to some manual intervention into the
              // database. Anyway, let's just leave such a build alone.
              //
              shared_ptr<build> b (build_db_->find<build> (bid));

              if (b == nullptr)
              {
                b = make_shared<build> (
                  move (bid.package.tenant),
                  move (bid.package.name),
                  bp.version,
                  move (bid.target),
                  move (bid.target_config_name),
                  move (bid.package_config_name),
                  move (bid.toolchain_name),
                  toolchain_version,
                  nullopt             /* interactive */,
                  nullopt             /* agent_fp */,
                  nullopt             /* agent_challenge */,
                  "brep"              /* machine */,
                  "build task module" /* machine_summary */,
                  ""                  /* controller_checksum */,
                  ""                  /* machine_checksum */);

                b->state  = build_state::built;
                b->status = result_status::abort;

                b->soft_timestamp = b->timestamp;
                b->hard_timestamp = b->soft_timestamp;

                // Mark the section as loaded, so results are updated.
                //
                b->results_section.load ();

                b->results.push_back (
                  operation_result {
                    "configure",
                      result_status::abort,
                      "error: multiple configurations for interactive build\n"});

                build_db_->persist (b);
              }
            }

            // Archive the tenant.
            //
            t->archived = true;
            build_db_->update (t);

            continue; // Skip the package.
          }
        }

        for (build_package_config& pc: p->configs)
        {
          pkg_config_name = pc.name;

          // Iterate through the built configurations and erase them from the
          // build configuration map. All those configurations that remained
          // can be built. We will take the first one, if present.
          //
          // Also save the built configurations for which it's time to be
          // rebuilt.
          //
          config_machines configs (conf_machines); // Make a copy for this pkg.
          auto pkg_builds (bld_prep_query.execute ());

          for (auto i (pkg_builds.begin ()); i != pkg_builds.end (); ++i)
          {
            auto j (
              configs.find (build_target_config_id {i->id.target,
                                                    i->id.target_config_name}));

            // Outdated configurations are already excluded with the database
            // query.
            //
            assert (j != configs.end ());
            configs.erase (j);

            if (i->state == build_state::built)
            {
              assert (i->force != force_state::forcing);

              if (needs_rebuild (*i))
                rebuilds.emplace_back (i.load ());
            }
          }

          if (!configs.empty ())
          {
            // Find the first build configuration that is not excluded by the
            // package configuration.
            //
            auto i (configs.begin ());
            auto e (configs.end ());

            for (;
                 i != e &&
                 exclude (pc, p->builds, p->constraints, *i->second.config);
                 ++i) ;

            if (i != e)
            {
              config_machine& cm (i->second);
              machine_header_manifest& mh (*cm.machine);

              build_id bid (move (id),
                            cm.config->target,
                            cm.config->name,
                            move (pkg_config_name),
                            move (tqm.toolchain_name),
                            toolchain_version);

              shared_ptr<build> b (build_db_->find<build> (bid));
              optional<string> cl (challenge ());

              shared_ptr<build_tenant> t (
                build_db_->load<build_tenant> (bid.package.tenant));

              // Move the interactive build login information into the build
              // object, if the package to be built interactively.
              //
              optional<string> login (t->interactive
                                      ? move (tqm.interactive_login)
                                      : nullopt);

              // If build configuration doesn't exist then create the new one
              // and persist. Otherwise put it into the building state, refresh
              // the timestamp and update.
              //
              if (b == nullptr)
              {
                b = make_shared<build> (move (bid.package.tenant),
                                        move (bid.package.name),
                                        move (bp.version),
                                        move (bid.target),
                                        move (bid.target_config_name),
                                        move (bid.package_config_name),
                                        move (bid.toolchain_name),
                                        move (toolchain_version),
                                        move (login),
                                        move (agent_fp),
                                        move (cl),
                                        mh.name,
                                        move (mh.summary),
                                        controller_checksum (*cm.config),
                                        machine_checksum (*cm.machine));

                build_db_->persist (b);
              }
              else
              {
                // The build configuration is in the building state.
                //
                // Note that in both cases we keep the status intact to be
                // able to compare it with the final one in the result request
                // handling in order to decide if to send the notification
                // email. The same is true for the forced flag (in the sense
                // that we don't set the force state to unforced).
                //
                assert (b->state == build_state::building);

                b->state = build_state::building;
                b->interactive = move (login);

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

                string ccs (controller_checksum (*cm.config));
                string mcs (machine_checksum (*cm.machine));

                // Issue the hard rebuild if it is forced or the configuration
                // or machine has changed.
                //
                if (b->hard_timestamp <= hard_rebuild_expiration ||
                    b->force == force_state::forced              ||
                    b->controller_checksum != ccs                ||
                    b->machine_checksum != mcs)
                  convert_to_hard (b);

                b->controller_checksum = move (ccs);
                b->machine_checksum    = move (mcs);

                b->timestamp = system_clock::now ();

                build_db_->update (b);
              }

              // Archive an interactive tenant.
              //
              if (t->interactive)
              {
                t->archived = true;
                build_db_->update (t);
              }

              // Finally, prepare the task response manifest.
              //
              tsm = task (move (b), move (p), move (pc), move (t), cm);
              break; // Bail out from the package configurations loop.
            }
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

    // If we don't have an unbuilt package, then let's see if we have a build
    // configuration to rebuild.
    //
    if (tsm.session.empty () && !rebuilds.empty ())
    {
      // Sort the configuration rebuild list with the following sort priority:
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

      // Pick the first build configuration from the ordered list.
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

          if (b != nullptr                   &&
              b->state == build_state::built &&
              needs_rebuild (*b))
          {
            auto i (conf_machines.find (
                      build_target_config_id {b->target,
                                              b->target_config_name}));

            // Only actual package configurations are loaded (see above).
            //
            assert (i != conf_machines.end ());
            const config_machine& cm (i->second);

            // Rebuild the package if still present, is buildable, doesn't
            // exclude the configuration, and matches the request's
            // interactive mode.
            //
            // Note that while change of the latter seems rather far fetched,
            // let's check it for good measure.
            //
            shared_ptr<build_package> p (
              build_db_->find<build_package> (b->id.package));

            shared_ptr<build_tenant> t (
              p != nullptr
              ? build_db_->load<build_tenant> (p->id.tenant)
              : nullptr);

            build_package_config* pc (p != nullptr
                                      ? find (b->package_config_name,
                                              p->configs)
                                      : nullptr);

            if (pc != nullptr                          &&
                p->buildable                           &&
                (imode == interactive_mode::both ||
                 (t->interactive.has_value () ==
                  (imode == interactive_mode::true_))) &&
                !exclude (*pc, p->builds, p->constraints, *cm.config))
            {
              assert (b->status);

              b->state = build_state::building;

              // Save the interactive build login information into the build
              // object, if the package to be built interactively.
              //
              // Can't move from, as may need it on the next iteration.
              //
              b->interactive = t->interactive
                               ? tqm.interactive_login
                               : nullopt;

              // Can't move from, as may need them on the next iteration.
              //
              b->agent_fingerprint = agent_fp;
              b->agent_challenge = cl;

              const machine_header_manifest& mh (*cm.machine);
              b->machine = mh.name;
              b->machine_summary = mh.summary;

              // Issue the hard rebuild if the timeout expired, rebuild is
              // forced, or the configuration or machine has changed.
              //
              // Note that we never reset the build status (see above for the
              // reasoning).
              //
              string ccs (controller_checksum (*cm.config));
              string mcs (machine_checksum (*cm.machine));

              if (b->hard_timestamp <= hard_rebuild_expiration ||
                  b->force == force_state::forced              ||
                  b->controller_checksum != ccs                ||
                  b->machine_checksum != mcs)
                convert_to_hard (b);

              b->controller_checksum = move (ccs);
              b->machine_checksum    = move (mcs);

              b->timestamp = system_clock::now ();

              build_db_->update (b);

              tsm = task (move (b), move (p), move (*pc), move (t), cm);
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
