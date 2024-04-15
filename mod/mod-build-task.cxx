// file      : mod/mod-build-task.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-build-task.hxx>

#include <map>
#include <regex>
#include <chrono>
#include <random>

#include <odb/database.hxx>
#include <odb/transaction.hxx>
#include <odb/schema-catalog.hxx>

#include <libbutl/ft/lang.hxx> // thread_local

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

#include <mod/build.hxx>               // send_notification_email()
#include <mod/module-options.hxx>
#include <mod/build-target-config.hxx>

using namespace std;
using namespace butl;
using namespace bbot;
using namespace brep::cli;
using namespace odb::core;

static thread_local mt19937 rand_gen (random_device {} ());

// Generate a random number in the specified range (max value is included).
//
static inline size_t
rand (size_t min_val, size_t max_val)
{
  // Note that size_t is not whitelisted as a type the
  // uniform_int_distribution class template can be instantiated with.
  //
  return min_val == max_val
         ? min_val
         : static_cast<size_t> (
             uniform_int_distribution<unsigned long long> (
               static_cast<unsigned long long> (min_val),
               static_cast<unsigned long long> (max_val)) (rand_gen));
}

brep::build_task::
build_task (const tenant_service_map& tsm)
    : tenant_service_map_ (tsm)
{
}

// While currently the user-defined copy constructor is not required (we don't
// need to deep copy nullptr's), it is a good idea to keep the placeholder
// ready for less trivial cases.
//
brep::build_task::
build_task (const build_task& r, const tenant_service_map& tsm)
    : database_module (r),
      build_config_module (r),
      options_ (r.initialized_ ? r.options_ : nullptr),
      tenant_service_map_ (tsm)
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

// Skip tenants with the freshly queued packages from the consideration (see
// tenant::queued_timestamp for the details on the service notifications race
// prevention).
//
template <typename T>
static inline query<T>
package_query (bool custom_bot,
               brep::params::build_task& params,
               interactive_mode imode,
               uint64_t queued_expiration_ns)
{
  using namespace brep;
  using query = query<T>;

  query q (!query::build_tenant::archived);

  if (custom_bot)
  {
    // Note that we could potentially only query the packages which refer to
    // this custom bot key in one of their build configurations. For that we
    // would need to additionally join the current query tables with the bot
    // fingerprint-containing build_package_bot_keys and
    // build_package_config_bot_keys tables and use the SELECT DISTINCT
    // clause. The problem is that we also use the ORDER BY clause and in this
    // case PostgreSQL requires all the ORDER BY clause expressions to also be
    // present in the SELECT DISTINCT clause and fails with the 'for SELECT
    // DISTINCT, ORDER BY expressions must appear in select list' error if
    // that's not the case. Also note that in the ODB-generated code the
    // 'build_package.project::TEXT' expression in the SELECT DISTINCT clause
    // (see the CITEXT type mapping for details in libbrep/common.hxx) would
    // not match the 'build_package.name' expression in the ORDER BY clause
    // and so we will end up with the mentioned error. One (hackish) way to
    // fix that would be to add a dummy member of the string type for the
    // build_package.name column. This all sounds quite hairy at the moment
    // and it also feels that this can potentially pessimize querying the
    // packages built with the default bots only. Thus let's keep it simple
    // for now and filter packages by the bot fingerprint at the program
    // level.
    //
    q = q && (query::build_package::custom_bot.is_null () ||
              query::build_package::custom_bot);
  }
  else
    q = q && (query::build_package::custom_bot.is_null () ||
              !query::build_package::custom_bot);

  // Filter by repositories canonical names (if requested).
  //
  const strings& rp (params.repository ());

  if (!rp.empty ())
    q = q &&
      query::build_repository::id.canonical_name.in_range (rp.begin (),
                                                           rp.end ());

  // If the interactive mode is false or true, then filter out the respective
  // packages.
  //
  switch (imode)
  {
  case interactive_mode::false_:
    {
      q = q && query::build_tenant::interactive.is_null ();
      break;
      }
  case interactive_mode::true_:
    {
      q = q && query::build_tenant::interactive.is_not_null ();
      break;
    }
  case interactive_mode::both: break;
  }

  return q &&
         (query::build_tenant::queued_timestamp.is_null () ||
          query::build_tenant::queued_timestamp < queued_expiration_ns);
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

  // Obtain the agent's public key fingerprint if requested. If the
  // fingerprint is requested but is not present in the request, then respond
  // with 401 HTTP code (unauthorized). If a key with the specified
  // fingerprint is not present in the build bot agent keys directory, then
  // assume that this is a custom build bot.
  //
  // Note that if the agent authentication is not configured (the agent keys
  // directory is not specified), then the bot can never be custom and its
  // fingerprint is ignored, if present.
  //
  optional<string> agent_fp;
  bool custom_bot (false);

  if (bot_agent_key_map_ != nullptr)
  {
    if (!tqm.fingerprint)
      throw invalid_request (401, "unauthorized");

    agent_fp = move (tqm.fingerprint);

    custom_bot = (bot_agent_key_map_->find (*agent_fp) ==
                  bot_agent_key_map_->end ());
  }

  // The resulting task manifest and the related build, package, and
  // configuration objects. Note that the latter 3 are only meaningful if the
  // the task manifest is present.
  //
  task_response_manifest      task_response;
  shared_ptr<build>           task_build;
  shared_ptr<build_package>   task_package;
  const build_package_config* task_config;

  auto serialize_task_response_manifest = [&task_response, &rs] ()
  {
    // @@ Probably it would be a good idea to also send some cache control
    //    headers to avoid caching by HTTP proxies. That would require
    //    extension of the web::response interface.
    //

    manifest_serializer s (rs.content (200, "text/manifest;charset=utf-8"),
                           "task_response_manifest");
    task_response.serialize (s);
  };

  interactive_mode imode (tqm.effective_interactive_mode ());

  // Restict the interactive mode (specified by the task request manifest) if
  // the interactive parameter is specified and is other than "both". If
  // values specified by the parameter and manifest are incompatible (false vs
  // true), then just bail out responding with the manifest with an empty
  // session.
  //
  if (params.interactive () != interactive_mode::both)
  {
    if (imode != interactive_mode::both)
    {
      if (params.interactive () != imode)
      {
        serialize_task_response_manifest ();
        return true;
      }
    }
    else
      imode = params.interactive (); // Can only change both to true or false.
  }

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

  for (const build_target_config& c: *target_conf_)
  {
    for (machine_header_manifest& m: tqm.machines)
    {
      if (m.effective_role () == machine_role::build)
      try
      {
        // The same story as in exclude() from build-target-config.cxx.
        //
        if (path_match (dash_components_to_path (m.name),
                        dash_components_to_path (c.machine_pattern),
                        dir_path () /* start */,
                        path_match_flags::match_absent))
        {
          conf_machines.emplace (build_target_config_id {c.target, c.name},
                                 config_machine {&c, &m});
          break;
        }
      }
      catch (const invalid_path&) {}
    }
  }

  // Collect the auxiliary configurations/machines available for the build.
  //
  struct auxiliary_config_machine
  {
    string config;
    const machine_header_manifest* machine;
  };

  vector<auxiliary_config_machine> auxiliary_config_machines;

  for (const machine_header_manifest& m: tqm.machines)
  {
    if (m.effective_role () == machine_role::auxiliary)
    {
      // Derive the auxiliary configuration name by stripping the first
      // (architecture) component from the machine name.
      //
      size_t p (m.name.find ('-'));

      if (p == string::npos || p == 0 || p == m.name.size () - 1)
        throw invalid_request (400,
                               (string ("no ") +
                                (p == 0 ? "architecture" : "OS") +
                                " component in machine name '" + m.name + "'"));

      auxiliary_config_machines.push_back (
        auxiliary_config_machine {string (m.name, p + 1), &m});
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
    auto task = [this] (const build& b,
                        const build_package& p,
                        const build_package_config& pc,
                        small_vector<bpkg::test_dependency, 1>&& tests,
                        vector<auxiliary_machine>&& ams,
                        optional<string>&& interactive,
                        const config_machine& cm) -> task_response_manifest
    {
      uint64_t ts (
        chrono::duration_cast<std::chrono::nanoseconds> (
          b.timestamp.time_since_epoch ()).count ());

      string session (b.tenant + '/'                      +
                      b.package_name.string () + '/'      +
                      b.package_version.string () + '/'   +
                      b.target.string () + '/'            +
                      b.target_config_name + '/'          +
                      b.package_config_name + '/'         +
                      b.toolchain_name + '/'              +
                      b.toolchain_version.string () + '/' +
                      to_string (ts));

      string tenant (tenant_dir (options_->root (), b.tenant).string ());
      string result_url (options_->host () + tenant + "?build-result");

      assert (transaction::has_current ());

      assert (p.internal ()); // The package is expected to be buildable.

      shared_ptr<build_repository> r (p.internal_repository.load ());

      strings fps;
      if (r->certificate_fingerprint)
        fps.emplace_back (move (*r->certificate_fingerprint));

      const package_name& pn (p.id.name);

      bool module_pkg (pn.string ().compare (0, 10, "libbuild2-") == 0);

      // Note that the auxiliary environment is crafted by the bbot agent
      // after the auxiliary machines are booted.
      //
      task_manifest task (pn,
                          p.version,
                          move (r->location),
                          move (fps),
                          p.requirements,
                          move (tests),
                          b.dependency_checksum,
                          cm.machine->name,
                          move (ams),
                          cm.config->target,
                          cm.config->environment,
                          nullopt /* auxiliary_environment */,
                          cm.config->args,
                          pc.arguments,
                          belongs (*cm.config, module_pkg ? "build2" : "host"),
                          cm.config->warning_regexes,
                          move (interactive),
                          b.worker_checksum);

      // Collect the build artifacts upload URLs, skipping those which are
      // excluded with the upload-*-exclude configuration options.
      //
      vector<upload_url> upload_urls;

      for (const auto& ud: options_->upload_data ())
      {
        const string& t (ud.first);

        auto exclude = [&t] (const multimap<string, string>& mm,
                             const string& v)
        {
          auto range (mm.equal_range (t));

          for (auto i (range.first); i != range.second; ++i)
          {
            if (i->second == v)
              return true;
          }

          return false;
        };

        if (!exclude (options_->upload_toolchain_exclude (),
                      b.toolchain_name) &&
            !exclude (options_->upload_repository_exclude (),
                      r->canonical_name))
        {
          upload_urls.emplace_back (options_->host () + tenant + "?upload=" + t,
                                    t);
        }
      }

      return task_response_manifest (move (session),
                                     b.agent_challenge,
                                     move (result_url),
                                     move (upload_urls),
                                     b.agent_checksum,
                                     move (task));
    };

    // Calculate the build/rebuild (building/built state) and the `queued`
    // notifications expiration time for package configurations.
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

    uint64_t queued_expiration_ns (
      expiration_ns (options_->build_queued_timeout ()));

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
    string&       toolchain_name (tqm.toolchain_name);

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

    pkg_query pq (package_query<buildable_package> (custom_bot,
                                                    params,
                                                    imode,
                                                    queued_expiration_ns));

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

    // In the random package ordering mode iterate over the packages list by
    // starting from the random offset and wrapping around when reaching the
    // end.
    //
    // Note, however, that since there can be some packages which are already
    // built for all configurations and are not archived yet, picking an
    // unbuilt package this way may not work as desired. Think of the
    // following case with 5 packages in 3 non-archived tenants:
    //
    // 0: A - unbuilt, tenant 1
    // 1: B - built,   tenant 2
    // 2: C - built,   tenant 2
    // 3: D - built,   tenant 2
    // 4: E - unbuilt, tenant 3
    //
    // If we just pick a random starting offset in the [0, 4] range, then we
    // will build A package with probability 0.2 and E with probability 0.8.
    //
    // To fix that we will only try to build a package from a tenant that the
    // random starting offset refers to. Failed that, we will randomly pick
    // new starting offset and retry. To make sure we don't retry indefinitely
    // when there are no more packages to build (and also for the sake of
    // optimization; see below), we will track positions of packages which we
    // (unsuccessfully) have already tried to build and skip them while
    // generating the random starting offsets and while iterating over
    // packages.
    //
    // Also note that since we iterate over packages in chunks, each queried
    // in a separate transaction, the number of packages may potentially
    // increase or decrease while iterating over them. Thus, to keep things
    // consistent, we may need to update our tried positions tracking state
    // accordingly (not to cycle, not to refer to an entry out of the list
    // boundaries, etc). Generally, regardless whether the number of packages
    // has changed or not, the offsets and position statuses may now refer to
    // some different packages. The only sensible thing we can do in such
    // cases (without trying to detect this situation and restart from
    // scratch) is to serve the request and issue some build task, if
    // possible.
    //
    bool random (options_->build_package_order () == build_order::random);
    size_t start_offset (0);

    // List of "tried to build" package statuses. True entries denote
    // positions of packages which we have tried to build. Initially all
    // entries are false.
    //
    vector<bool> tried_positions;

    // Number of false entries in the above vector. Used merely as an
    // optimization to bail out.
    //
    size_t untried_positions_count (0);

    // Return a random position of a package that we have not yet tried to
    // build, if present, and nullopt otherwise.
    //
    auto rand_position = [&tried_positions,
                          &untried_positions_count] () -> optional<size_t>
    {
      assert (untried_positions_count <= tried_positions.size ());

      if (untried_positions_count == 0)
        return nullopt;

      size_t r;
      while (tried_positions[r = rand (0, tried_positions.size () - 1)]) ;
      return r;
    };

    // Mark the package at specified position as tried to build. Assume that
    // it is not yet been tried to build.
    //
    auto position_tried = [&tried_positions,
                           &untried_positions_count] (size_t i)
    {
      assert (i < tried_positions.size () &&
              !tried_positions[i]         &&
              untried_positions_count != 0);

      tried_positions[i] = true;
      --untried_positions_count;
    };

    // Resize the tried positions list and update the untried positions
    // counter accordingly if the package number has changed.
    //
    // For simplicity, assume that packages are added/removed to/from the end
    // of the list. Note that misguessing in such a rare cases are possible
    // but not harmful (see above for the reasoning).
    //
    auto resize_tried_positions = [&tried_positions, &untried_positions_count]
                                  (size_t n)
    {
      if (n > tried_positions.size ())      // Packages added?
      {
        untried_positions_count += n - tried_positions.size ();
        tried_positions.resize (n, false);
      }
      else if (n < tried_positions.size ()) // Packages removed?
      {
        for (size_t i (n); i != tried_positions.size (); ++i)
        {
          if (!tried_positions[i])
          {
            assert (untried_positions_count != 0);
            --untried_positions_count;
          }
        }

        tried_positions.resize (n);
      }
      else
      {
        // Not supposed to be called if the number of packages didn't change.
        //
        assert (false);
      }
    };

    if (random)
    {
      using query = query<buildable_package_count>;

      query q (package_query<buildable_package_count> (custom_bot,
                                                       params,
                                                       imode,
                                                       queued_expiration_ns));

      transaction t (build_db_->begin ());

      // If there are any non-archived interactive build tenants, then the
      // chosen randomization approach doesn't really work since interactive
      // tenants must be preferred over non-interactive ones, which is
      // achieved by proper ordering of the package query result (see below).
      // Thus, we just disable randomization if there are any interactive
      // tenants.
      //
      // But shouldn't we randomize the order between packages in multiple
      // interactive tenants? Given that such a tenant may only contain a
      // single package and can only be built in a single configuration that
      // is probably not important. However, we may assume that the
      // randomization still happens naturally due to the random nature of the
      // tenant id, which is used as a primary sorting criteria (see below).
      //
      size_t interactive_package_count (
        build_db_->query_value<buildable_package_count> (
          q && query::build_tenant::interactive.is_not_null ()));

      if (interactive_package_count == 0)
      {
        untried_positions_count =
          build_db_->query_value<buildable_package_count> (q);
      }
      else
        random = false;

      t.commit ();

      if (untried_positions_count != 0)
      {
        tried_positions.resize (untried_positions_count, false);

        optional<size_t> so (rand_position ());
        assert (so); // Wouldn't be here otherwise.
        start_offset = *so;
      }
    }

    if (!random || !tried_positions.empty ())
    {
      // Specify the portion.
      //
      size_t offset (start_offset);
      size_t limit  (50);

      pq += "ORDER BY";

      // If the interactive mode is both, then order the packages so that ones
      // from the interactive build tenants appear first.
      //
      if (imode == interactive_mode::both)
        pq += pkg_query::build_tenant::interactive + "NULLS LAST,";

      pq += pkg_query::build_package::id.tenant + ","                  +
        pkg_query::build_package::id.name                              +
        order_by_version (pkg_query::build_package::id.version, false) +
        "OFFSET" + pkg_query::_ref (offset)                            +
        "LIMIT" + pkg_query::_ref (limit);

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
      // This is why we query the database for configurations that should not
      // be built (in the built state, or in the building state and not
      // expired). Having such a list we will select the first build
      // configuration that is not in the list (if available) for the
      // response.
      //
      using bld_query = query<build>;
      using prep_bld_query = prepared_query<build>;

      package_id id;
      string pkg_config;

      bld_query sq (false);
      for (const auto& cm: conf_machines)
        sq = sq || (bld_query::id.target == cm.first.target &&
                    bld_query::id.target_config_name == cm.first.config);

      bld_query bq (
        equal<build> (bld_query::id.package, id)                          &&
        bld_query::id.package_config_name == bld_query::_ref (pkg_config) &&
        sq                                                                &&
        bld_query::id.toolchain_name == toolchain_name                    &&

        compare_version_eq (bld_query::id.toolchain_version,
                            canonical_version (toolchain_version),
                            true /* revision */)                          &&

        (bld_query::state == "built" ||
         (bld_query::state == "building" &&
          ((bld_query::force == "forcing" &&
            bld_query::timestamp > forced_result_expiration_ns) ||
           (bld_query::force != "forcing" && // Unforced or forced.
            bld_query::timestamp > normal_result_expiration_ns)))));

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

      // Convert a build to the hard rebuild, resetting the agent checksum.
      //
      // Note that since the checksums are hierarchical, the agent checksum
      // reset will trigger resets of the "subordinate" checksums up to the
      // dependency checksum and so the package will be rebuilt.
      //
      // Also note that we keep the previous build task result and status
      // intact since we may still need to revert the build into the built
      // state if the task execution is interrupted.
      //
      auto convert_to_hard = [] (const shared_ptr<build>& b)
      {
        b->agent_checksum = nullopt;
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
      // Note that we don't include auxiliary machine ids into this checksum
      // since a different machine will most likely get picked for a pattern.
      // And we view all auxiliary machines that match a pattern as equal for
      // testing purposes (in other words, pattern is not the way to get
      // coverage).
      //
      auto machine_checksum = [] (const machine_header_manifest& m)
      {
        return m.id;
      };

      // Tenant that the start offset refers to.
      //
      optional<string> start_tenant;

      // If the build task is created and the tenant of the being built
      // package has a third-party service state associated with it, then
      // check if the tenant_service_build_building and/or
      // tenant_service_build_queued callbacks are registered for the type of
      // the associated service. If they are, then stash the state, the build
      // object, and the callback pointers for the subsequent service
      // notifications.
      //
      // Also, if the tenant_service_build_queued callback is registered, then
      // create, persist, and stash the queued build objects for all the
      // unbuilt by the current toolchain and not yet queued configurations of
      // the package the build task is created for and calculate the hints.
      // Note that for the task build, we need to make sure that the
      // third-party service receives the `queued` notification prior to the
      // `building` notification (see mod/tenant-service.hxx for valid
      // transitions). The `queued` notification is assumed to be already sent
      // for the build if the respective object exists and any of the
      // following is true for it:
      //
      // - It is in the queued state (initial_state is build_state::queued).
      //
      // - It is a user-forced rebuild of an incomplete build
      //   (rebuild_forced_build is true).
      //
      // - It is a rebuild of an interrupted rebuild (rebuild_forced_build is
      //   true).
      //
      const tenant_service_build_building* tsb (nullptr);
      const tenant_service_build_queued* tsq (nullptr);
      optional<pair<tenant_service, shared_ptr<build>>> tss;
      vector<build> qbs;
      tenant_service_build_queued::build_queued_hints qhs;
      optional<build_state> initial_state;
      bool rebuild_forced_build (false);
      bool rebuild_interrupted_rebuild (false);

      // Create, persist, and return the queued build objects for all the
      // unbuilt by the current toolchain and not yet queued configurations of
      // the specified package.
      //
      // Note that the build object argument is only used for the toolchain
      // information retrieval. Also note that the package constraints section
      // is expected to be loaded.
      //
      auto queue_builds = [this] (const build_package& p, const build& b)
      {
        assert (p.constraints_section.loaded ());

        // Query the existing build ids and stash them into the set.
        //
        set<build_id> existing_builds;

        using query = query<package_build_id>;

        query q (query::build::id.package == p.id                     &&
                 query::build::id.toolchain_name == b.toolchain_name  &&
                 compare_version_eq (query::build::id.toolchain_version,
                                     b.id.toolchain_version,
                                     true /* revision */));

        for (build_id& id: build_db_->query<package_build_id> (q))
          existing_builds.emplace (move (id));

        // Go through all the potential package builds and queue those which
        // are not in the existing builds set.
        //
        vector<build> r;

        for (const build_package_config& pc: p.configs)
        {
          for (const build_target_config& tc: *target_conf_)
          {
            if (!exclude (pc, p.builds, p.constraints, tc))
            {
              build_id id (p.id,
                           tc.target, tc.name,
                           pc.name,
                           b.toolchain_name, b.toolchain_version);

              if (existing_builds.find (id) == existing_builds.end ())
              {
                r.emplace_back (move (id.package.tenant),
                                move (id.package.name),
                                p.version,
                                move (id.target),
                                move (id.target_config_name),
                                move (id.package_config_name),
                                move (id.toolchain_name),
                                b.toolchain_version);

                // @@ TODO Persist the whole vector of builds with a single
                //         operation if/when bulk operations support is added
                //         for objects with containers.
                //
                build_db_->persist (r.back ());
              }
            }
          }
        }

        return r;
      };

      auto queue_hints = [this] (const build_package& p)
      {
        buildable_package_count tpc (
          build_db_->query_value<buildable_package_count> (
            query<buildable_package_count>::build_tenant::id == p.id.tenant));

        return tenant_service_build_queued::build_queued_hints {
          tpc == 1, p.configs.size () == 1};
      };

      // Collect the auxiliary machines required for testing of the specified
      // package configuration and the external test packages, if present for
      // the specified target configuration (task_auxiliary_machines),
      // together with the auxiliary machines information that needs to be
      // persisted in the database as a part of the build object
      // (build_auxiliary_machines, which is parallel to
      // task_auxiliary_machines). While at it collect the involved test
      // dependencies. Return nullopt if any auxiliary configuration patterns
      // may not be resolved to the auxiliary machines (no matching
      // configuration, auxiliary machines RAM limit is exceeded, etc).
      //
      // Note that if the same auxiliary environment name is used for multiple
      // packages (for example, for the main and tests packages or for the
      // tests and examples packages, etc), then a shared auxiliary machine is
      // used for all these packages. In this case all the respective
      // configuration patterns must match the configuration derived from this
      // machine name. If they don't, then return nullopt. The thinking here
      // is that on the next task request a machine whose derived
      // configuration matches all the patterns can potentially be picked.
      //
      struct collect_auxiliaries_result
      {
        vector<auxiliary_machine>              task_auxiliary_machines;
        vector<build_machine>                  build_auxiliary_machines;
        small_vector<bpkg::test_dependency, 1> tests;
      };

      auto collect_auxiliaries = [&tqm, &auxiliary_config_machines, this]
                                 (const shared_ptr<build_package>& p,
                                  const build_package_config& pc,
                                  const build_target_config& tc)
        -> optional<collect_auxiliaries_result>
      {
        // The list of the picked build auxiliary machines together with the
        // environment names they have been picked for.
        //
        vector<pair<auxiliary_config_machine, string>> picked_machines;

        // Try to randomly pick the auxiliary machine that matches the
        // specified pattern and which can be supplied with the minimum
        // required RAM, if specified. Return false if such a machine is not
        // available. If a machine is already picked for the specified
        // environment name, then return true if the machine's configuration
        // matches the specified pattern and false otherwise.
        //
        auto pick_machine =
          [&tqm,
           &picked_machines,
           used_ram = uint64_t (0),
           available_machines = auxiliary_config_machines]
          (const build_auxiliary& ba) mutable -> bool
        {
          vector<size_t> ams; // Indexes of the available matching machines.
          optional<uint64_t> ar (tqm.auxiliary_ram);

          // If the machine configuration name pattern (which is legal) or any
          // of the machine configuration names (illegal) are invalid paths,
          // then we assume we cannot pick the machine.
          //
          try
          {
            // The same story as in exclude() from build-target-config.cxx.
            //
            auto match = [pattern = dash_components_to_path (ba.config)]
                         (const string& config)
            {
              return path_match (dash_components_to_path (config),
                                 pattern,
                                 dir_path () /* start */,
                                 path_match_flags::match_absent);
            };

            // Check if a machine is already picked for the specified
            // environment name.
            //
            for (const auto& m: picked_machines)
            {
              if (m.second == ba.environment_name)
                return match (m.first.config);
            }

            // Collect the matching machines from the list of the available
            // machines and bail out if there are none.
            //
            for (size_t i (0); i != available_machines.size (); ++i)
            {
              const auxiliary_config_machine& m (available_machines[i]);
              optional<uint64_t> mr (m.machine->ram_minimum);

              if (match (m.config) && (!mr || !ar || used_ram + *mr <= *ar))
                ams.push_back (i);
            }

            if (ams.empty ())
              return false;
          }
          catch (const invalid_path&)
          {
            return false;
          }

          // Pick the matching machine randomly.
          //
          size_t i (ams[rand (0, ams.size () - 1)]);
          auxiliary_config_machine& cm (available_machines[i]);

          // Bump the used RAM.
          //
          if (optional<uint64_t> r = cm.machine->ram_minimum)
            used_ram += *r;

          // Move out the picked machine from the available machines list.
          //
          picked_machines.emplace_back (move (cm), ba.environment_name);
          available_machines.erase (available_machines.begin () + i);
          return true;
        };

        // Collect auxiliary machines for the main package build configuration.
        //
        for (const build_auxiliary& ba:
               pc.effective_auxiliaries (p->auxiliaries))
        {
          if (!pick_machine (ba))
            return nullopt; // No matched auxiliary machine.
        }

        // Collect the test packages and the auxiliary machines for their
        // default build configurations. Exclude external test packages which
        // exclude the current target configuration.
        //
        small_vector<bpkg::test_dependency, 1> tests;

        if (!p->requirements_tests_section.loaded ())
          build_db_->load (*p, p->requirements_tests_section);

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
            shared_ptr<build_package> tp (td.package.load ());

            // Try to use the test package configuration named the same as the
            // current configuration of the main package. If there is no such
            // a configuration, then fallback to using the default
            // configuration (which must exist). If the selected test package
            // configuration excludes the current target configuration, then
            // exclude this external test package from the build task.
            //
            // Note that potentially the selected test package configuration
            // may contain some (bpkg) arguments associated, but we currently
            // don't provide build bot worker with such information. This,
            // however, is probably too far fetched so let's keep it simple
            // for now.
            //
            const build_package_config* tpc (find (pc.name, tp->configs));

            if (tpc == nullptr)
            {
              tpc = find ("default", tp->configs);

              assert (tpc != nullptr); // Must always be present.
            }

            // Use the `all` class as a least restrictive default underlying
            // build class set. Note that we should only apply the explicit
            // build restrictions to the external test packages (think about
            // the `builds: all` and `builds: -windows` manifest values for
            // the primary and external test packages, respectively).
            //
            build_db_->load (*tp, tp->constraints_section);

            if (exclude (*tpc,
                         tp->builds,
                         tp->constraints,
                         tc,
                         nullptr /* reason */,
                         true /* default_all_ucs */))
              continue;

            build_db_->load (*tp, tp->auxiliaries_section);

            for (const build_auxiliary& ba:
                   tpc->effective_auxiliaries (tp->auxiliaries))
            {
              if (!pick_machine (ba))
                return nullopt; // No matched auxiliary machine.
            }
          }

          tests.emplace_back (td.name,
                              td.type,
                              td.buildtime,
                              td.constraint,
                              td.enable,
                              td.reflect);
        }

        vector<auxiliary_machine> tms;
        vector<build_machine> bms;

        if (size_t n = picked_machines.size ())
        {
          tms.reserve (n);
          bms.reserve (n);

          for (pair<auxiliary_config_machine, string>& pm: picked_machines)
          {
            const machine_header_manifest& m (*pm.first.machine);
            tms.push_back (auxiliary_machine {m.name, move (pm.second)});
            bms.push_back (build_machine {m.name, m.summary});
          }
        }

        return collect_auxiliaries_result {
          move (tms), move (bms), move (tests)};
      };

      // While at it, collect the aborted for various reasons builds
      // (interactive builds in multiple configurations, builds with too many
      // auxiliary machines, etc) to send the notification emails at the end
      // of the request handling.
      //
      struct aborted_build
      {
        shared_ptr<build>           b;
        shared_ptr<build_package>   p;
        const build_package_config* pc;
        const char*                 what;
      };
      vector<aborted_build> aborted_builds;

      // Note: is only used for crafting of the notification email subjects.
      //
      bool unforced (true);

      for (bool done (false); !task_response.task && !done; )
      {
        transaction tr (conn->begin ());

        // We need to be careful in the random package ordering mode not to
        // miss the end after having wrapped around.
        //
        done = (start_offset != 0     &&
                offset < start_offset &&
                offset + limit >= start_offset);

        if (done)
          limit = start_offset - offset;

        // Query (and cache) buildable packages.
        //
        auto packages (pkg_prep_query.execute ());

        size_t chunk_size (packages.size ());
        size_t next_offset (offset + chunk_size);

        // If we are in the random package ordering mode, then also check if
        // the package number has changed and, if that's the case, resize the
        // tried positions list accordingly.
        //
        if (random &&
            (next_offset > tried_positions.size () ||
             (next_offset < tried_positions.size () && chunk_size < limit)))
        {
          resize_tried_positions (next_offset);
        }

        // Bail out if there is nothing left, unless we need to wrap around in
        // the random package ordering mode.
        //
        if (chunk_size == 0)
        {
          tr.commit ();

          if (start_offset != 0 && offset >= start_offset)
            offset = 0;
          else
            done = true;

          continue;
        }

        size_t position (offset); // Current package position.
        offset = next_offset;

        // Iterate over packages until we find one that needs building or have
        // to bail out in the random package ordering mode for some reason (no
        // more untried positions, need to restart, etc).
        //
        // Note that it is not uncommon for the sequentially examined packages
        // to belong to the same tenant (single tenant mode, etc). Thus, we
        // will cache the loaded tenant objects.
        //
        shared_ptr<build_tenant> t;

        for (auto& bp: packages)
        {
          shared_ptr<build_package>& p (bp.package);

          id = p->id;

          // Reset the tenant cache if the current package belongs to a
          // different tenant.
          //
          if (t != nullptr && t->id != id.tenant)
            t = nullptr;

          // If we are in the random package ordering mode, then cache the
          // tenant the start offset refers to, if not cached yet, and check
          // if we are still iterating over packages from this tenant
          // otherwise. If the latter is not the case, then restart from a new
          // random untried offset, if present, and bail out otherwise.
          //
          if (random)
          {
            if (!start_tenant)
            {
              start_tenant = id.tenant;
            }
            else if (*start_tenant != id.tenant)
            {
              if (optional<size_t> so = rand_position ())
              {
                start_offset = *so;
                offset = start_offset;
                start_tenant = nullopt;
                limit = 50;
                done = false;
              }
              else
                done = true;

              break;
            }

            size_t pos (position++);

            // Should have been resized, if required.
            //
            assert (pos < tried_positions.size ());

            // Skip the position if it has already been tried.
            //
            if (tried_positions[pos])
              continue;

            position_tried (pos);
          }

          // Note that a request to interactively build a package in multiple
          // configurations is most likely a mistake than a deliberate choice.
          // Thus, for the interactive tenant let's check if the package can
          // be built in multiple configurations. If that's the case then we
          // will put all the potential builds into the aborted state and
          // continue iterating looking for another package. Otherwise, just
          // proceed for this package normally.
          //
          // It also feels like a good idea to archive an interactive tenant
          // after a build object is created for it, regardless if the build
          // task is issued or not. This way we make sure that an interactive
          // build is never performed multiple times for such a tenant for any
          // reason (multiple toolchains, buildtab change, etc). Note that the
          // build result will still be accepted for an archived build.
          //
          if (bp.interactive)
          {
            // Note that the tenant can be archived via some other package on
            // some previous iteration. Skip the package if that's the case.
            //
            // Also note that if bp.archived is false, then we need to
            // (re-)load the tenant object to re-check the archived flag.
            //
            if (!bp.archived)
            {
              if (t == nullptr)
                t = build_db_->load<build_tenant> (id.tenant);

              bp.archived = t->archived;
            }

            if (bp.archived)
              continue;

            assert (t != nullptr); // Wouldn't be here otherwise.

            // Collect the potential build configurations as all combinations
            // of the tenant's packages build configurations and the
            // non-excluded (by the packages) build target
            // configurations. Note that here we ignore the machines from the
            // task request.
            //
            struct build_config
            {
              shared_ptr<build_package>   p;
              const build_package_config* pc;
              const build_target_config*  tc;
            };

            small_vector<build_config, 1> build_configs;

            // Note that we don't bother creating a prepared query here, since
            // its highly unlikely to encounter multiple interactive tenants
            // per task request. Given that we archive such tenants
            // immediately, as a common case there will be none.
            //
            pkg_query pq (pkg_query::build_tenant::id == id.tenant);
            for (auto& tp: build_db_->query<buildable_package> (pq))
            {
              shared_ptr<build_package>& p (tp.package);

              build_db_->load (*p, p->constraints_section);

              for (build_package_config& pc: p->configs)
              {
                for (const auto& tc: *target_conf_)
                {
                  if (!exclude (pc, p->builds, p->constraints, tc))
                    build_configs.push_back (build_config {p, &pc, &tc});
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
                shared_ptr<build_package>& p (c.p);
                const string& pc (c.pc->name);
                const build_target_config& tc (*c.tc);

                build_id bid (p->id,
                              tc.target,
                              tc.name,
                              pc,
                              toolchain_name,
                              toolchain_version);

                // Can there be any existing builds for such a tenant? Doesn't
                // seem so, unless due to some manual intervention into the
                // database. Anyway, let's just leave such a build alone.
                //
                shared_ptr<build> b (build_db_->find<build> (bid));

                if (b == nullptr)
                {
                  b = make_shared<build> (move (bid.package.tenant),
                                          move (bid.package.name),
                                          p->version,
                                          move (bid.target),
                                          move (bid.target_config_name),
                                          move (bid.package_config_name),
                                          move (bid.toolchain_name),
                                          toolchain_version,
                                          result_status::abort,
                                          operation_results ({
                                            operation_result {
                                              "configure",
                                              result_status::abort,
                                              "error: multiple configurations "
                                              "for interactive build\n"}}),
                                          build_machine {
                                            "brep", "build task module"});

                  build_db_->persist (b);

                  // Schedule the build notification email.
                  //
                  aborted_builds.push_back (aborted_build {
                      move (b), move (p), c.pc, "build"});
                }
              }

              // Archive the tenant.
              //
              t->archived = true;
              build_db_->update (t);

              continue; // Skip the package.
            }
          }

          // If true, then the package is (being) built for some
          // configurations.
          //
          // Note that since we only query the built and forced rebuild
          // objects there can be false negatives.
          //
          bool package_built (false);

          build_db_->load (*p, p->bot_keys_section);

          for (const build_package_config& pc: p->configs)
          {
            // If this is a custom bot, then skip this configuration if it
            // doesn't contain this bot's public key in its custom bot keys
            // list. Otherwise (this is a default bot), skip this
            // configuration if its custom bot keys list is not empty.
            //
            {
              const build_package_bot_keys& bks (
                pc.effective_bot_keys (p->bot_keys));

              if (custom_bot)
              {
                assert (agent_fp); // Wouldn't be here otherwise.

                if (find_if (
                      bks.begin (), bks.end (),
                      [&agent_fp] (const lazy_shared_ptr<build_public_key>& k)
                      {
                        return k.object_id ().fingerprint == *agent_fp;
                      }) == bks.end ())
                {
                  continue;
                }
              }
              else
              {
                if (!bks.empty ())
                  continue;
              }
            }

            pkg_config = pc.name;

            // Iterate through the built configurations and erase them from the
            // build configuration map. All those configurations that remained
            // can be built. We will take the first one, if present.
            //
            // Also save the built configurations for which it's time to be
            // rebuilt.
            //
            config_machines configs (conf_machines); // Make copy for this pkg.
            auto pkg_builds (bld_prep_query.execute ());

            if (!package_built && !pkg_builds.empty ())
              package_built = true;

            for (auto i (pkg_builds.begin ()); i != pkg_builds.end (); ++i)
            {
              auto j (
                configs.find (build_target_config_id {
                    i->id.target, i->id.target_config_name}));

              // Outdated configurations are already excluded with the
              // database query.
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
              // Find the first build configuration that is not excluded by
              // the package configuration and for which all the requested
              // auxiliary machines can be provided.
              //
              const config_machine* cm (nullptr);
              optional<collect_auxiliaries_result> aux;

              build_db_->load (*p, p->constraints_section);

              for (auto i (configs.begin ()), e (configs.end ()); i != e; ++i)
              {
                cm = &i->second;
                const build_target_config& tc (*cm->config);

                if (!exclude (pc, p->builds, p->constraints, tc))
                {
                  if (!p->auxiliaries_section.loaded ())
                    build_db_->load (*p, p->auxiliaries_section);

                  if ((aux = collect_auxiliaries (p, pc, tc)))
                    break;
                }
              }

              if (aux)
              {
                machine_header_manifest& mh (*cm->machine);

                build_id bid (move (id),
                              cm->config->target,
                              cm->config->name,
                              move (pkg_config),
                              move (toolchain_name),
                              toolchain_version);

                shared_ptr<build> b (build_db_->find<build> (bid));
                optional<string> cl (challenge ());

                // Move the interactive build login information into the build
                // object, if the package to be built interactively.
                //
                optional<string> login (bp.interactive
                                        ? move (tqm.interactive_login)
                                        : nullopt);

                // If build configuration doesn't exist then create the new
                // one and persist. Otherwise put it into the building state,
                // refresh the timestamp and update.
                //
                if (b == nullptr)
                {
                  b = make_shared<build> (move (bid.package.tenant),
                                          move (bid.package.name),
                                          p->version,
                                          move (bid.target),
                                          move (bid.target_config_name),
                                          move (bid.package_config_name),
                                          move (bid.toolchain_name),
                                          move (toolchain_version),
                                          move (login),
                                          move (agent_fp),
                                          move (cl),
                                          build_machine {
                                            mh.name, move (mh.summary)},
                                          move (aux->build_auxiliary_machines),
                                          controller_checksum (*cm->config),
                                          machine_checksum (*cm->machine));

                  build_db_->persist (b);
                }
                else
                {
                  // The build configuration is in the building or queued
                  // state.
                  //
                  // Note that in both the building and built cases we keep
                  // the status intact to be able to compare it with the final
                  // one in the result request handling in order to decide if
                  // to send the notification email or to revert it to the
                  // built state if interrupted. The same is true for the
                  // forced flag (in the sense that we don't set the force
                  // state to unforced).
                  //
                  assert (b->state != build_state::built);

                  initial_state = b->state;

                  b->state = build_state::building;
                  b->interactive = move (login);

                  unforced = (b->force == force_state::unforced);

                  // Switch the force state not to reissue the task after the
                  // forced rebuild timeout. Note that the result handler will
                  // still recognize that the rebuild was forced.
                  //
                  if (b->force == force_state::forcing)
                  {
                    b->force = force_state::forced;
                    rebuild_forced_build = true;
                  }

                  b->agent_fingerprint = move (agent_fp);
                  b->agent_challenge = move (cl);
                  b->machine = build_machine {mh.name, move (mh.summary)};

                  // Mark the section as loaded, so auxiliary_machines are
                  // updated.
                  //
                  b->auxiliary_machines_section.load ();

                  b->auxiliary_machines =
                    move (aux->build_auxiliary_machines);

                  string ccs (controller_checksum (*cm->config));
                  string mcs (machine_checksum (*cm->machine));

                  // Issue the hard rebuild if it is forced or the
                  // configuration or machine has changed.
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

                if (t == nullptr)
                  t = build_db_->load<build_tenant> (b->tenant);

                // Archive an interactive tenant.
                //
                if (bp.interactive)
                {
                  t->archived = true;
                  build_db_->update (t);
                }

                // Finally, stash the service notification information, if
                // present, and prepare the task response manifest.
                //
                if (t->service)
                {
                  auto i (tenant_service_map_.find (t->service->type));

                  if (i != tenant_service_map_.end ())
                  {
                    const tenant_service_base* s (i->second.get ());

                    tsb = dynamic_cast<const tenant_service_build_building*> (s);
                    tsq = dynamic_cast<const tenant_service_build_queued*> (s);

                    if (tsq != nullptr)
                    {
                      qbs = queue_builds (*p, *b);

                      // If we ought to call the
                      // tenant_service_build_queued::build_queued() callback,
                      // then also set the package tenant's queued timestamp
                      // to the current time to prevent the notifications race
                      // (see tenant::queued_timestamp for details).
                      //
                      if (!qbs.empty ()  ||
                          !initial_state ||
                          (*initial_state != build_state::queued &&
                           !rebuild_forced_build))
                      {
                        qhs = queue_hints (*p);

                        t->queued_timestamp = system_clock::now ();
                        build_db_->update (t);
                      }
                    }

                    if (tsb != nullptr || tsq != nullptr)
                      tss = make_pair (*t->service, b);
                  }
                }

                task_response = task (*b,
                                      *p,
                                      pc,
                                      move (aux->tests),
                                      move (aux->task_auxiliary_machines),
                                      move (bp.interactive),
                                      *cm);

                task_build = move (b);
                task_package = move (p);
                task_config = &pc;

                package_built = true;

                break; // Bail out from the package configurations loop.
              }
            }
          }

          // If the task manifest is prepared, then bail out from the package
          // loop, commit the transaction and respond. Otherwise, stash the
          // build toolchain into the tenant, unless it is already stashed or
          // the current package already has some configurations (being)
          // built.
          //
          if (!task_response.task)
          {
            // Note that since there can be false negatives for the
            // package_built flag (see above), there can be redundant tenant
            // queries which, however, seems harmless (query uses the primary
            // key and the object memory footprint is small).
            //
            if (!package_built)
            {
              if (t == nullptr)
                t = build_db_->load<build_tenant> (p->id.tenant);

              if (!t->toolchain)
              {
                t->toolchain = build_toolchain {toolchain_name,
                                                toolchain_version};

                build_db_->update (t);
              }
            }
          }
          else
            break;
        }

        tr.commit ();
      }

      // If we don't have an unbuilt package, then let's see if we have a
      // build configuration to rebuild.
      //
      if (!task_response.task && !rebuilds.empty ())
      {
        // Sort the configuration rebuild list with the following sort
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

          // Older build completion goes first.
          //
          // Note that a completed build can have the state change timestamp
          // (timestamp member) newer than the completion timestamp
          // (soft_timestamp member) if the build was interrupted.
          //
          return x->soft_timestamp < y->soft_timestamp;
        };

        sort (rebuilds.begin (), rebuilds.end (), cmp);

        optional<string> cl (challenge ());

        // Pick the first build configuration from the ordered list.
        //
        // Note that the configurations and packages may not match the
        // required criteria anymore (as we have committed the database
        // transactions that were used to collect this data) so we recheck. If
        // we find one that matches then put it into the building state,
        // refresh the timestamp and update. Note that we don't amend the
        // status and the force state to have them available in the result
        // request handling (see above).
        //
        for (auto& b: rebuilds)
        {
          try
          {
            transaction t (conn->begin ());

            b = build_db_->find<build> (b->id);

            if (b != nullptr                   &&
                b->state == build_state::built &&
                needs_rebuild (*b))
            {
              auto i (conf_machines.find (
                        build_target_config_id {
                          b->target, b->target_config_name}));

              // Only actual package configurations are loaded (see above).
              //
              assert (i != conf_machines.end ());
              const config_machine& cm (i->second);

              // Rebuild the package configuration if still present, is
              // buildable, doesn't exclude the target configuration, can be
              // provided with all the requested auxiliary machines, and
              // matches the request's interactive mode.
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

              if (pc != nullptr &&
                  p->buildable  &&
                  (imode == interactive_mode::both ||
                   (t->interactive.has_value () ==
                    (imode == interactive_mode::true_))))
              {
                const build_target_config& tc (*cm.config);

                build_db_->load (*p, p->constraints_section);

                if (exclude (*pc, p->builds, p->constraints, tc))
                  continue;

                build_db_->load (*p, p->auxiliaries_section);

                if (optional<collect_auxiliaries_result> aux =
                    collect_auxiliaries (p, *pc, tc))
                {
                  assert (b->status);

                  initial_state = build_state::built;

                  rebuild_interrupted_rebuild =
                    (b->timestamp > b->soft_timestamp);

                  b->state = build_state::building;

                  // Save the interactive build login information into the
                  // build object, if the package to be built interactively.
                  //
                  // Can't move from, as may need it on the next iteration.
                  //
                  b->interactive = t->interactive
                    ? tqm.interactive_login
                    : nullopt;

                  unforced = (b->force == force_state::unforced);

                  // Can't move from, as may need them on the next iteration.
                  //
                  b->agent_fingerprint = agent_fp;
                  b->agent_challenge = cl;

                  const machine_header_manifest& mh (*cm.machine);
                  b->machine = build_machine {mh.name,  mh.summary};

                  // Mark the section as loaded, so auxiliary_machines are
                  // updated.
                  //
                  b->auxiliary_machines_section.load ();

                  b->auxiliary_machines =
                    move (aux->build_auxiliary_machines);

                  // Issue the hard rebuild if the timeout expired, rebuild is
                  // forced, or the configuration or machine has changed.
                  //
                  // Note that we never reset the build status (see above for
                  // the reasoning).
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

                  // Stash the service notification information, if present,
                  // and prepare the task response manifest.
                  //
                  if (t->service)
                  {
                    auto i (tenant_service_map_.find (t->service->type));

                    if (i != tenant_service_map_.end ())
                    {
                      const tenant_service_base* s (i->second.get ());

                      tsb = dynamic_cast<const tenant_service_build_building*> (s);
                      tsq = dynamic_cast<const tenant_service_build_queued*> (s);

                      if (tsq != nullptr)
                      {
                        qbs = queue_builds (*p, *b);

                        // If we ought to call the
                        // tenant_service_build_queued::build_queued()
                        // callback, then also set the package tenant's queued
                        // timestamp to the current time to prevent the
                        // notifications race (see tenant::queued_timestamp
                        // for details).
                        //
                        if (!qbs.empty () || !rebuild_interrupted_rebuild)
                        {
                          qhs = queue_hints (*p);

                          t->queued_timestamp = system_clock::now ();
                          build_db_->update (t);
                        }
                      }

                      if (tsb != nullptr || tsq != nullptr)
                        tss = make_pair (move (*t->service), b);
                    }
                  }

                  task_response = task (*b,
                                        *p,
                                        *pc,
                                        move (aux->tests),
                                        move (aux->task_auxiliary_machines),
                                        move (t->interactive),
                                        cm);

                  task_build = move (b);
                  task_package = move (p);
                  task_config = pc;
                }
              }
            }

            t.commit ();
          }
          catch (const odb::deadlock&)
          {
            // Just try with the next rebuild. But first, reset the task
            // manifest and the session that we may have prepared.
            //
            task_response = task_response_manifest ();
          }

          // If the task manifest is prepared, then bail out from the package
          // configuration rebuilds loop and respond.
          //
          if (task_response.task)
            break;
        }
      }

      // If the tenant-associated third-party service needs to be notified
      // about the queued builds, then call the
      // tenant_service_build_queued::build_queued() callback function and
      // update the service state, if requested.
      //
      if (tsq != nullptr)
      {
        assert (tss); // Wouldn't be here otherwise.

        const tenant_service& ss (tss->first);

        // If the task build has no initial state (is just created), then
        // temporarily move it into the list of the queued builds until the
        // `queued` notification is delivered. Afterwards, restore it so that
        // the `building` notification can also be sent.
        //
        build& b (*tss->second);
        bool restore_build (false);

        if (!initial_state)
        {
          qbs.push_back (move (b));
          restore_build = true;
        }

        if (!qbs.empty ())
        {
          if (auto f = tsq->build_queued (ss,
                                          qbs,
                                          nullopt /* initial_state */,
                                          qhs,
                                          log_writer_))
            update_tenant_service_state (conn, qbs.back ().tenant, f);
        }

        // Send the `queued` notification for the task build, unless it is
        // already sent, and update the service state, if requested.
        //
        if (initial_state                         &&
            *initial_state != build_state::queued &&
            !rebuild_interrupted_rebuild          &&
            !rebuild_forced_build)
        {
          qbs.clear ();
          qbs.push_back (move (b));
          restore_build = true;

          if (auto f = tsq->build_queued (ss,
                                          qbs,
                                          initial_state,
                                          qhs,
                                          log_writer_))
            update_tenant_service_state (conn, qbs.back ().tenant, f);
        }

        if (restore_build)
          b = move (qbs.back ());
      }

      // If a third-party service needs to be notified about the package
      // build, then call the tenant_service_build_built::build_building()
      // callback function and, if requested, update the tenant-associated
      // service state.
      //
      if (tsb != nullptr)
      {
        assert (tss); // Wouldn't be here otherwise.

        const tenant_service& ss (tss->first);
        const build& b (*tss->second);

        if (auto f = tsb->build_building (ss, b, log_writer_))
          update_tenant_service_state (conn, b.tenant, f);
      }

      // If the task manifest is prepared, then check that the number of the
      // build auxiliary machines is less than 10. If that's not the case,
      // then turn the build into the built state with the abort status.
      //
      if (task_response.task &&
          task_response.task->auxiliary_machines.size () > 9)
      {
        // Respond with the no-task manifest.
        //
        task_response = task_response_manifest ();

        // If the package tenant has a third-party service state associated
        // with it, then check if the tenant_service_build_built callback is
        // registered for the type of the associated service. If it is, then
        // stash the state, the build object, and the callback pointer for the
        // subsequent service `built` notification.
        //
        const tenant_service_build_built* tsb (nullptr);
        optional<pair<tenant_service, shared_ptr<build>>> tss;
        {
          transaction t (conn->begin ());

          shared_ptr<build> b (build_db_->find<build> (task_build->id));

          // For good measure, check that the build object is in the building
          // state and has not been updated.
          //
          if (b->state == build_state::building &&
              b->timestamp == task_build->timestamp)
          {
            b->state  = build_state::built;
            b->status = result_status::abort;
            b->force  = force_state::unforced;

            // Cleanup the interactive build login information.
            //
            b->interactive = nullopt;

            // Cleanup the authentication data.
            //
            b->agent_fingerprint = nullopt;
            b->agent_challenge = nullopt;

            b->timestamp = system_clock::now ();
            b->soft_timestamp = b->timestamp;
            b->hard_timestamp = b->soft_timestamp;

            // Mark the section as loaded, so results are updated.
            //
            b->results_section.load ();

            b->results = operation_results ({
                operation_result {
                  "configure",
                  result_status::abort,
                  "error: not more than 9 auxiliary machines are allowed"}});

            b->agent_checksum      = nullopt;
            b->worker_checksum     = nullopt;
            b->dependency_checksum = nullopt;

            build_db_->update (b);

            // Schedule the `built` notification, if the
            // tenant_service_build_built callback is registered for the
            // tenant.
            //
            shared_ptr<build_tenant> t (
              build_db_->load<build_tenant> (b->tenant));

            if (t->service)
            {
              auto i (tenant_service_map_.find (t->service->type));

              if (i != tenant_service_map_.end ())
              {
                tsb = dynamic_cast<const tenant_service_build_built*> (
                  i->second.get ());

                // If required, stash the service notification information.
                //
                if (tsb != nullptr)
                  tss = make_pair (move (*t->service), b);
              }
            }

            // Schedule the build notification email.
            //
            aborted_builds.push_back (
              aborted_build {move (b),
                             move (task_package),
                             task_config,
                             unforced ? "build" : "rebuild"});
          }

          t.commit ();
        }

        // If a third-party service needs to be notified about the built
        // package, then call the tenant_service_build_built::build_built()
        // callback function and update the service state, if requested.
        //
        if (tsb != nullptr)
        {
          assert (tss); // Wouldn't be here otherwise.

          const tenant_service& ss (tss->first);
          const build& b (*tss->second);

          if (auto f = tsb->build_built (ss, b, log_writer_))
            update_tenant_service_state (conn, b.tenant, f);
        }
      }

      // Send notification emails for all the aborted builds.
      //
      for (const aborted_build& ab: aborted_builds)
        send_notification_email (*options_,
                                 conn,
                                 *ab.b,
                                 *ab.p,
                                 *ab.pc,
                                 ab.what,
                                 error,
                                 verb_ >= 2 ? &trace : nullptr);
    }
  }

  serialize_task_response_manifest ();
  return true;
}
