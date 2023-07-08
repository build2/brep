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

#include <mod/build-target-config.hxx>

#include <mod/module-options.hxx>

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
  return static_cast<size_t> (
    uniform_int_distribution<unsigned long long> (
      static_cast<unsigned long long> (min_val),
      static_cast<unsigned long long> (max_val)) (rand_gen));
}

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

template <typename T>
static inline query<T>
package_query (brep::params::build_task& params, interactive_mode imode)
{
  using namespace brep;
  using query = query<T>;

  query q (!query::build_tenant::archived);

  // Filter by repositories canonical names (if requested).
  //
  const vector<string>& rp (params.repository ());

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

  return q;
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

  auto serialize_task_response_manifest = [&tsm, &rs] ()
  {
    // @@ Probably it would be a good idea to also send some cache control
    //    headers to avoid caching by HTTP proxies. That would require
    //    extension of the web::response interface.
    //

    manifest_serializer s (rs.content (200, "text/manifest;charset=utf-8"),
                           "task_response_manifest");
    tsm.serialize (s);
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
        {
          conf_machines.emplace (build_target_config_id {c.target, c.name},
                                 config_machine {&c, &m});
          break;
        }
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
                        optional<string>&& interactive,
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

      string tenant (tenant_dir (options_->root (), b->tenant).string ());
      string result_url (options_->host () + tenant + "?build-result");

      assert (transaction::has_current ());

      assert (p->internal ()); // The package is expected to be buildable.

      shared_ptr<build_repository> r (p->internal_repository.load ());

      strings fps;
      if (r->certificate_fingerprint)
        fps.emplace_back (move (*r->certificate_fingerprint));

      // Exclude external test packages which exclude the task build
      // configuration.
      //
      small_vector<bpkg::test_dependency, 1> tests;

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
          build_db_->load (*p, p->constraints_section);

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
                          move (interactive),
                          move (b->worker_checksum));

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
                      b->toolchain_name) &&
            !exclude (options_->upload_repository_exclude (),
                      r->canonical_name))
        {
          upload_urls.emplace_back (options_->host () + tenant + "?upload=" + t,
                                    t);
        }
      }

      return task_response_manifest (move (session),
                                     move (b->agent_challenge),
                                     move (result_url),
                                     move (upload_urls),
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

    pkg_query pq (package_query<buildable_package> (params, imode));

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

      query q (package_query<buildable_package_count> (params, imode));

      transaction t (build_db_->begin ());

      // If there are any non-archived interactive build tenants, then the
      // chosen randomization approach doesn't really work since interactive
      // tenants must be preferred over non-interactive ones, which is
      // achieved by proper ordering of the package query result (see
      // below). Thus, we just disable randomization if there are any
      // interactive tenants.
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
      // expired).  Having such a list we will select the first build
      // configuration that is not in the list (if available) for the
      // response.
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
      auto machine_checksum = [] (const machine_header_manifest& m)
      {
        return m.id;
      };

      // Tenant that the start offset refers to.
      //
      optional<string> start_tenant;

      for (bool done (false); tsm.session.empty () && !done; )
      {
        transaction t (conn->begin ());

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
          t.commit ();

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
        for (auto& bp: packages)
        {
          shared_ptr<build_package>& p (bp.package);

          id = p->id;

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
            shared_ptr<build_tenant> t;

            // Note that the tenant can be archived via some other package on
            // some previous iteration. Skip the package if that's the case.
            //
            // Also note that if bp.archived is false, then we need to
            // (re-)load the tenant object to re-check the archived flag.
            //
            if (!bp.archived)
            {
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
              package_id                 pid;
              string                     pc;
              const build_target_config* tc;
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
                    build_configs.push_back (
                      build_config {p->id, pc.name, &tc});
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
                    p->version,
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
            config_machines configs (conf_machines); // Make copy for this pkg.
            auto pkg_builds (bld_prep_query.execute ());

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
              // the package configuration.
              //
              auto i (configs.begin ());
              auto e (configs.end ());

              build_db_->load (*p, p->constraints_section);

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
                  // able to compare it with the final one in the result
                  // request handling in order to decide if to send the
                  // notification email or to revert it to the built state if
                  // interrupted. The same is true for the forced flag (in
                  // the sense that we don't set the force state to unforced).
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

                // Archive an interactive tenant.
                //
                if (bp.interactive)
                {
                  shared_ptr<build_tenant> t (
                    build_db_->load<build_tenant> (b->id.package.tenant));

                  t->archived = true;
                  build_db_->update (t);
                }

                // Finally, prepare the task response manifest.
                //
                tsm = task (
                  move (b), move (p), move (pc), move (bp.interactive), cm);

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

      // If we don't have an unbuilt package, then let's see if we have a
      // build configuration to rebuild.
      //
      if (tsm.session.empty () && !rebuilds.empty ())
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
            transaction t (build_db_->begin ());

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

              if (pc != nullptr &&
                  p->buildable  &&
                  (imode == interactive_mode::both ||
                   (t->interactive.has_value () ==
                    (imode == interactive_mode::true_))))
              {
                build_db_->load (*p, p->constraints_section);

                if (!exclude (*pc, p->builds, p->constraints, *cm.config))
                {
                  assert (b->status);

                  b->state = build_state::building;

                  // Save the interactive build login information into the
                  // build object, if the package to be built interactively.
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

                  tsm = task (
                    move (b), move (p), move (*pc), move (t->interactive), cm);
                }
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
  }

  serialize_task_response_manifest ();
  return true;
}
