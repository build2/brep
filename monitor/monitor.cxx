// file      : monitor/monitor.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <map>
#include <set>
#include <chrono>
#include <iostream>

#include <odb/database.hxx>
#include <odb/transaction.hxx>
#include <odb/schema-catalog.hxx>

#include <odb/pgsql/database.hxx>

#include <libbutl/pager.hxx>

#include <libbrep/build.hxx>
#include <libbrep/common.hxx>
#include <libbrep/build-odb.hxx>
#include <libbrep/build-package.hxx>
#include <libbrep/build-package-odb.hxx>
#include <libbrep/database-lock.hxx>

#include <mod/build-target-config.hxx>

#include <monitor/module-options.hxx>
#include <monitor/monitor-options.hxx>

using namespace std;
using namespace butl;
using namespace odb::core;

namespace brep
{
  // Operation failed, diagnostics has already been issued.
  //
  struct failed {};

  // We will collect and report build delays as separate steps not to hold
  // database locks while printing to stderr. Also we need to order delays
  // properly, so while printing reports we could group delays by toolchain
  // and target configuration.
  //
  // To achieve that, we will iterate through all possible package builds
  // creating the list of delays with the following sort priority:
  //
  // 1: toolchain name
  // 2: toolchain version (descending)
  // 3: target configuration name
  // 4: target
  // 5: tenant
  // 6: package name
  // 7: package version (descending)
  // 8: package configuration name
  //
  struct compare_delay
  {
    bool
    operator() (const shared_ptr<const build_delay>& x,
                const shared_ptr<const build_delay>& y) const
    {
      if (int r = x->toolchain_name.compare (y->toolchain_name))
        return r < 0;

      if (int r = x->toolchain_version.compare (y->toolchain_version))
        return r > 0;

      if (int r = x->target_config_name.compare (y->target_config_name))
        return r < 0;

      if (int r = x->target.compare (y->target))
        return r < 0;

      if (int r = x->tenant.compare (y->tenant))
        return r < 0;

      if (int r = x->package_name.compare (y->package_name))
        return r < 0;

      if (int r = x->package_version.compare (y->package_version))
        return r > 0;

      return x->package_config_name.compare (y->package_config_name) < 0;
    }
  };

  // The ordered list of delays to report.
  //
  class delay_report
  {
  public:
    // Note that in the brief mode we also need to print the total number of
    // delays (reported or not) per target configuration. Thus, we add all
    // delays to the report object, marking them if we need to report them or
    // not.
    //
    void
    add_delay (shared_ptr<build_delay>, bool report);

    bool
    empty () const {return reported_delay_count_ == 0;}

    // In the brief mode (if full is false) print the number of reported/total
    // (if total is true) delayed package configuration builds per target
    // configuration rather than the package configurations themselves.
    //
    void
    print (const char* header, bool total, bool full) const;

  private:
    // Maps delays to the report flag.
    //
    map<shared_ptr<const build_delay>, bool, compare_delay> delays_;
    size_t reported_delay_count_ = 0;
  };

  void delay_report::
  add_delay (shared_ptr<build_delay> delay, bool report)
  {
    delays_.emplace (move (delay), report);

    if (report)
      ++reported_delay_count_;
  }

  void delay_report::
  print (const char* header, bool total, bool full) const
  {
    if (empty ())
      return;

    cerr << header << " (" << reported_delay_count_;

    if (total)
      cerr << '/' << delays_.size ();

    cerr << "):" << endl;

    // Group the printed delays by toolchain and target configuration.
    //
    const string*         toolchain_name     (nullptr);
    const version*        toolchain_version  (nullptr);
    const string*         target_config_name (nullptr);
    const target_triplet* target             (nullptr);

    size_t config_reported_delay_count (0);
    size_t config_total_delay_count (0);

    auto brief_config = [&target_config_name,
                         &target,
                         &config_reported_delay_count,
                         &config_total_delay_count,
                         total] ()
    {
      if (target_config_name != nullptr)
      {
        assert (target != nullptr);

        // Only print configurations with delays that needs to be reported.
        //
        if (config_reported_delay_count != 0)
        {
          cerr << "    " << *target_config_name << '/' << *target << " ("
               << config_reported_delay_count;

          if (total)
            cerr << '/' << config_total_delay_count;

          cerr << ')' << endl;
        }

        config_reported_delay_count = 0;
        config_total_delay_count = 0;
      }
    };

    for (const auto& dr: delays_)
    {
      bool report (dr.second);

      if (full && !report)
        continue;

      const shared_ptr<const build_delay>& d (dr.first);

      // Print the toolchain, if changed.
      //
      if (toolchain_name == nullptr            ||
          d->toolchain_name != *toolchain_name ||
          d->toolchain_version != *toolchain_version)
      {
        if (!full)
          brief_config ();

        if (toolchain_name != nullptr)
          cerr << endl;

        cerr << "  " << d->toolchain_name;

        if (!d->toolchain_version.empty ())
          cerr << "/" << d->toolchain_version;

        cerr << endl;

        toolchain_name     = &d->toolchain_name;
        toolchain_version  = &d->toolchain_version;
        target_config_name = nullptr;
        target             = nullptr;
      }

      // Print the configuration, if changed.
      //
      if (target_config_name == nullptr                ||
          d->target_config_name != *target_config_name ||
          d->target != *target)
      {
        if (full)
        {
          if (target_config_name != nullptr)
            cerr << endl;

          cerr << "    " << d->target_config_name << '/' << d->target << endl;
        }
        else
          brief_config ();

        target_config_name = &d->target_config_name;
        target             = &d->target;
      }

      // Print the delayed build package configuration in the full report mode
      // and count configuration builds otherwise.
      //
      if (full)
      {
        // We can potentially extend this information with the archived flag
        // or the delay duration.
        //
        cerr << "      " << d->package_name << '/' << d->package_version
             << ' ' << d->package_config_name;

        if (!d->tenant.empty ())
          cerr << ' ' << d->tenant;

        cerr << endl;
      }
      else
      {
        if (report)
          ++config_reported_delay_count;

        ++config_total_delay_count;
      }
    }

    if (!full)
      brief_config ();
  }

  static const char* help_info (
    "  info: run 'brep-monitor --help' for more information");

  static int
  main (int argc, char* argv[])
  try
  {
    cli::argv_scanner scan (argc, argv);
    options::monitor ops (scan);

    // Version.
    //
    if (ops.version ())
    {
      cout << "brep-monitor " << BREP_VERSION_ID << endl
           << "libbrep " << LIBBREP_VERSION_ID << endl
           << "libbbot " << LIBBBOT_VERSION_ID << endl
           << "libbpkg " << LIBBPKG_VERSION_ID << endl
           << "libbutl " << LIBBUTL_VERSION_ID << endl
           << "Copyright (c) " << BREP_COPYRIGHT << "." << endl
           << "This is free software released under the MIT license." << endl;

      return 0;
    }

    // Help.
    //
    if (ops.help ())
    {
      pager p ("brep-monitor help",
               false,
               ops.pager_specified () ? &ops.pager () : nullptr,
               &ops.pager_option ());

      print_usage (p.stream ());

      // If the pager failed, assume it has issued some diagnostics.
      //
      return p.wait () ? 0 : 1;
    }

    // Parse the brep module configuration.
    //
    options::module mod_ops;
    {
      if (!scan.more ())
      {
        cerr << "error: brep module configuration file is expected" << endl
             << help_info << endl;
        return 1;
      }

      string f (scan.next ());

      try
      {
        cli::argv_file_scanner scan (f, "" /* option */);

        // Parse the brep module options skipping those we don't recognize.
        //
        while (scan.more ())
        {
          // Parse until an unknown option is encountered.
          //
          mod_ops.parse (scan,
                         cli::unknown_mode::stop,
                         cli::unknown_mode::stop);

          // Skip the unknown option, unless we are done.
          //
          if (scan.more ())
          {
            // Skip the option name.
            //
            size_t l (scan.peek_line ());
            scan.skip ();

            // Skip the option value, if present.
            //
            // Note that here we rely on the configuration file having both
            // the option name and its value on the same line.
            //
            if (scan.more () && scan.peek_line () == l)
              scan.skip ();
          }
        }
      }
      catch (const cli::file_io_failure& e)
      {
        cerr << "error: unable to parse brep module configuration: " << e
             << endl;
        return 1;
      }
      catch (const cli::exception& e)
      {
        cerr << "error: unable to parse brep module configuration file '" << f
             << "': " << e << endl;
        return 1;
      }

      auto bad_alt = [&f] (const char* what)
      {
        cerr << "build-alt-" << what << "-rebuild-start and build-alt-"
             << what << "-rebuild-stop configuration options must both be "
             << "either specified or not in '" << f << "'" << endl;
      };

      if (mod_ops.build_alt_hard_rebuild_start_specified () !=
          mod_ops.build_alt_hard_rebuild_stop_specified ())
      {
        bad_alt("hard");
        return 1;
      }

      if (mod_ops.build_alt_soft_rebuild_start_specified () !=
          mod_ops.build_alt_soft_rebuild_stop_specified ())
      {
        bad_alt("soft");
        return 1;
      }
    }

    // Parse the toolchains suppressing duplicates.
    //
    // Note that specifying a toolchain both with and without version doesn't
    // make sense, so we fail if that's the case.
    //
    vector<pair<string, version>> toolchains;

    if (!scan.more ())
    {
      cerr << "error: toolchain is expected" << endl << help_info << endl;
      return 1;
    }

    while (scan.more ())
    {
      string s (scan.next ());

      string tn;
      version tv;

      try
      {
        size_t p (s.find ('/'));

        if (p == string::npos)
          tn = move (s);
        else
        {
          tn.assign (s, 0, p);
          tv = version (string (s, p + 1));
        }

        bool dup (false);
        for (const pair<string, version>& t: toolchains)
        {
          if (tn == t.first)
          {
            if (tv == t.second)
            {
              dup = true;
              break;
            }

            if (tv.empty () != t.second.empty ())
            {
              cerr << "error: toolchain '" << tn << "' is specified both "
                   << "with and without version" << endl;
              return 1;
            }
          }
        }

        if (!dup)
          toolchains.emplace_back (move (tn), move (tv));
      }
      catch (const invalid_argument& e)
      {
        cerr << "error: invalid toolchain '" << s << "': " << e << endl;
        return 1;
      }
    }

    // Parse buildtab.
    //
    if (!mod_ops.build_config_specified ())
    {
      cerr << "warning: package building functionality is disabled" << endl;
      return 0;
    }

    build_target_configs configs;

    try
    {
      configs = bbot::parse_buildtab (mod_ops.build_config ());
    }
    catch (const tab_parsing& e)
    {
      cerr << "error: unable to parse buildtab: " << e << endl;
      return 1;
    }
    catch (const io_error& e)
    {
      cerr << "error: unable to read '" << mod_ops.build_config () << "': "
           << e << endl;
      return 1;
    }

    // Create the database instance.
    //
    odb::pgsql::database db (
      ops.build_db_user (),
      ops.build_db_password (),
      (ops.build_db_name_specified ()
       ? ops.build_db_name ()
       : mod_ops.build_db_name ()),
      (ops.build_db_host_specified ()
       ? ops.build_db_host ()
       : mod_ops.build_db_host ()),
      (ops.build_db_port_specified ()
       ? ops.build_db_port ()
       : mod_ops.build_db_port ()),
      "options='-c default_transaction_isolation=serializable'");

    // Prevent several brep utility instances from updating the build database
    // simultaneously.
    //
    database_lock l (db);

    // Check that the database schema matches the current one.
    //
    const string ds ("build");
    if (schema_catalog::current_version (db, ds) != db.schema_version (ds))
    {
      cerr << "error: build database schema differs from the current one"
           << endl
           << "  info: use brep-migrate to migrate the database" << endl;
      return 1;
    }

    // If requested, cleanup delays for package builds that are not expected
    // anymore (build configuration is not present, etc).
    //
    if (ops.clean ())
    {
      using config_map = map<build_target_config_id,
                             const build_target_config*>;

      config_map conf_map;
      for (const build_target_config& c: configs)
        conf_map[build_target_config_id {c.target, c.name}] = &c;

      // Prepare the build delay prepared query.
      //
      // Query package build delays in chunks in order not to hold locks for
      // too long. Sort the result by package version as a first priority to
      // minimize number of queries to the package database. Note that we
      // still need to sort by configuration and toolchain to make sure that
      // build delays are sorted consistently across queries and we don't miss
      // any of them.
      //
      using query = query<build_delay>;
      using prep_query = prepared_query<build_delay>;

      // Specify the portion.
      //
      size_t offset (0);

      query q ("ORDER BY" +
               query::id.package.tenant + ","             +
               query::id.package.name                     +
               order_by_version (query::id.package.version,
                                 false /* first */) + "," +
               query::id.target + ","                     +
               query::id.target_config_name + ","         +
               query::id.package_config_name + ","        +
               query::id.toolchain_name                   +
               order_by_version (query::id.toolchain_version,
                                 false /* first */)       +
               "OFFSET" + query::_ref (offset) + "LIMIT 2000");

      connection_ptr conn (db.connection ());

      prep_query pq (
        conn->prepare_query<build_delay> ("build-delay-query", q));

      // Cache the delayed build package object to reuse it in case the next
      // delay refers to the same package (which is often the case due to the
      // query result sorting criteria we use).
      //
      package_id pid;
      shared_ptr<build_package> p;

      for (bool ne (true); ne; )
      {
        transaction t (conn->begin ());

        // Query delays.
        //
        auto delays (pq.execute ());

        if ((ne = !delays.empty ()))
        {
          // Iterate over the build delays and cleanup the outdated ones.
          //
          for (const build_delay& d: delays)
          {
            config_map::const_iterator ci;

            bool cleanup (
              // Check that the toolchain is still used.
              //
              find_if (toolchains.begin (), toolchains.end (),
                       [&d] (const pair<string, version>& t)
                       {
                         return t.first == d.toolchain_name &&
                                t.second == d.toolchain_version;
                       }) == toolchains.end () ||
              //
              // Check that the build configuration is still present.
              //
              (ci = conf_map.find (
                build_target_config_id {d.target,
                                        d.target_config_name})) ==
              conf_map.end ());

            // Check that the package still present, is buildable and doesn't
            // exclude the build configuration.
            //
            if (!cleanup)
            {
              if (d.id.package != pid)
              {
                pid = d.id.package;
                p = db.find<build_package> (pid);
              }

              const build_package_config* pc (p != nullptr
                                              ? find (d.package_config_name,
                                                      p->configs)
                                              : nullptr);

              cleanup = (pc == nullptr || !p->buildable);

              if (!cleanup)
              {
                db.load (*p, p->constraints_section);

                cleanup = exclude (*pc,
                                   p->builds,
                                   p->constraints,
                                   *ci->second,
                                   configs.class_inheritance_map);
              }
            }

            if (cleanup)
              db.erase (d);
            else
              ++offset;
          }
        }

        t.commit ();
      }
    }

    delay_report hard_delays_report;
    delay_report soft_delays_report;
    set<shared_ptr<const build_delay>, compare_delay> update_delays;
    {
      connection_ptr conn (db.connection ());

      // Prepare the buildable package prepared query.
      //
      // Query buildable packages in chunks in order not to hold locks for too
      // long.
      //
      using pquery = query<buildable_package>;
      using prep_pquery = prepared_query<buildable_package>;

      // Specify the portion.
      //
      size_t offset (0);

      pquery pq ("ORDER BY"                            +
                 pquery::build_package::id.tenant + "," +
                 pquery::build_package::id.name         +
                 order_by_version (pquery::build_package::id.version,
                                   false /* first */)  +
                 "OFFSET" + pquery::_ref (offset) + "LIMIT 50");

      prep_pquery ppq (
        conn->prepare_query<buildable_package> ("buildable-package-query",
                                                pq));

      // Prepare the package configuration build prepared queries.
      //
      using bquery = query<build>;
      using prep_bquery = prepared_query<build>;

      build_id id;

      // This query will only be used for toolchains that have no version
      // specified on the command line to obtain the latest completed build
      // across all toolchain versions, if present, and the latest incomplete
      // build otherwise.
      //
      // Why don't we pick the latest toolchain version? We don't want to
      // stuck with it on the toolchain rollback. Instead we prefer the
      // toolchain that built the package last and if there are none, pick the
      // one for which the build task was issued last.
      //
      // @@ TMP Check if we can optimize this query by adding index for
      //        soft_timestamp and/or by setting enable_nestloop=off (or some
      //        such) as we do in mod/mod-builds.cxx.
      //
      bquery lbq ((equal<build> (bquery::id,
                                 id,
                                 false /* toolchain_version */) &&
                   bquery::state != "queued")       +
                  "ORDER BY"                        +
                  bquery::soft_timestamp + "DESC, " +
                  bquery::timestamp + "DESC"        +
                  "LIMIT 1");

      prep_bquery plbq (
        conn->prepare_query<build> ("package-latest-build-query", lbq));

      // This query will only be used to retrieve a specific build by id.
      //
      bquery bq (equal<build> (bquery::id, id) && bquery::state != "queued");
      prep_bquery pbq (conn->prepare_query<build> ("package-build-query", bq));

      timestamp now (system_clock::now ());

      // Calculate the build/rebuild expiration time, based on the respective
      // --{soft,hard}-rebuild-timeout monitor options and the
      // build-{soft,hard}-rebuild-timeout and
      // build-alt-{soft,hard}-rebuild-{start,stop,timeout} brep module
      // configuration options.
      //
      // If the --*-rebuild-timeout monitor option is zero or is not specified
      // and the respective build-*-rebuild-timeout brep's configuration
      // option is zero, then return timestamp_unknown to indicate 'never
      // expire'. Note that this value is less than any build timestamp value,
      // including timestamp_nonexistent.
      //
      // NOTE: there is a similar code in mod/mod-build-task.cxx.
      //
      auto build_expiration = [&now, &mod_ops] (
        optional<size_t> rebuild_timeout,
        const optional<pair<duration, duration>>& alt_interval,
        optional<size_t> alt_timeout,
        size_t normal_timeout)
      {
        duration t;

        // If the rebuild timeout is not specified explicitly, then calculate
        // it as the sum of the package rebuild timeout (normal rebuild
        // timeout if the alternative timeout is unspecified and the maximum
        // of two otherwise) and the build result timeout.
        //
        if (!rebuild_timeout)
        {
          if (normal_timeout == 0)
            return timestamp_unknown;

          chrono::seconds nt (normal_timeout);

          if (alt_interval)
          {
            // Calculate the alternative timeout, unless it is specified
            // explicitly.
            //
            if (!alt_timeout)
            {
              const duration& start (alt_interval->first);
              const duration& stop  (alt_interval->second);

              // Note that if the stop time is less than the start time then
              // the interval extends through the midnight.
              //
              t = start <= stop ? (stop - start) : ((24h - start) + stop);

              // If the normal rebuild time out is greater than 24 hours, then
              // increase the default alternative timeout by (normal - 24h)
              // (see build-alt-soft-rebuild-timeout configuration option for
              // details).
              //
              if (nt > 24h)
                t += nt - 24h;
            }
            else
              t = chrono::seconds (*alt_timeout);

            // Take the maximum of the alternative and normal rebuild
            // timeouts.
            //
            if (t < nt)
              t = nt;
          }
          else
            t = nt;

          // Summarize the rebuild and build result timeouts.
          //
          t += chrono::seconds (mod_ops.build_result_timeout ());
        }
        else
        {
          if (*rebuild_timeout == 0)
            return timestamp_unknown;

          t = chrono::seconds (*rebuild_timeout);
        }

        return now - t;
      };

      timestamp hard_rebuild_expiration (
        build_expiration (
          (ops.hard_rebuild_timeout_specified ()
           ? ops.hard_rebuild_timeout ()
           : optional<size_t> ()),
          (mod_ops.build_alt_hard_rebuild_start_specified ()
           ? make_pair (mod_ops.build_alt_hard_rebuild_start (),
                        mod_ops.build_alt_hard_rebuild_stop ())
           : optional<pair<duration, duration>> ()),
          (mod_ops.build_alt_hard_rebuild_timeout_specified ()
           ? mod_ops.build_alt_hard_rebuild_timeout ()
           : optional<size_t> ()),
          mod_ops.build_hard_rebuild_timeout ()));

      timestamp soft_rebuild_expiration (
        build_expiration (
          (ops.soft_rebuild_timeout_specified ()
           ? ops.soft_rebuild_timeout ()
           : optional<size_t> ()),
          (mod_ops.build_alt_soft_rebuild_start_specified ()
           ? make_pair (mod_ops.build_alt_soft_rebuild_start (),
                        mod_ops.build_alt_soft_rebuild_stop ())
           : optional<pair<duration, duration>> ()),
          (mod_ops.build_alt_soft_rebuild_timeout_specified ()
           ? mod_ops.build_alt_soft_rebuild_timeout ()
           : optional<size_t> ()),
          mod_ops.build_soft_rebuild_timeout ()));

      timestamp report_expiration (
        now - chrono::seconds (ops.report_timeout ()));

      for (bool ne (true); ne; )
      {
        transaction t (conn->begin ());

        // Query buildable packages (and cache the result).
        //
        auto bps (ppq.execute ());

        if ((ne = !bps.empty ()))
        {
          offset += bps.size ();

          for (auto& bp: bps)
          {
            shared_ptr<build_package>& p (bp.package);

            db.load (*p, p->constraints_section);

            for (const build_package_config& pc: p->configs)
            {
              for (const build_target_config& tc: configs)
              {
                if (exclude (pc,
                             p->builds,
                             p->constraints,
                             tc,
                             configs.class_inheritance_map))
                  continue;

                for (const pair<string, version>& t: toolchains)
                {
                  id = build_id (p->id,
                                 tc.target, tc.name,
                                 pc.name,
                                 t.first, t.second);

                  // If the toolchain version is unspecified then search for
                  // the latest build across all toolchain versions and search
                  // for a specific build otherwise.
                  //
                  shared_ptr<build> b (id.toolchain_version.empty ()
                                       ? plbq.execute_one ()
                                       : pbq.execute_one ());

                  // Note that we consider a build as delayed if it is not
                  // completed in the expected timeframe. So even if the build
                  // task have been issued recently we may still consider the
                  // build as delayed.
                  //
                  timestamp bht (b != nullptr
                                 ? b->hard_timestamp
                                 : timestamp_nonexistent);

                  timestamp bst (b != nullptr
                                 ? b->soft_timestamp
                                 : timestamp_nonexistent);

                  // Create the delay object to record a timestamp when the
                  // package configuration build could have potentially been
                  // started, unless it already exists.
                  //
                  shared_ptr<build_delay> d (db.find<build_delay> (id));

                  if (d == nullptr)
                  {
                    // If the archived package has no build nor build delay
                    // for this configuration, then we assume that the
                    // configuration was added after the package tenant has
                    // been archived and so the package could have never been
                    // built for this configuration. Thus, we don't consider
                    // this build as delayed and so skip it.
                    //
                    if (bp.archived && b == nullptr)
                      continue;

                    // Use the build hard, soft, or status change timestamp
                    // (see the timestamps description for their ordering
                    // information) as the build delay tracking starting point
                    // and fallback to the current time if there is no build
                    // yet.
                    //
                    timestamp pts (b == nullptr                 ? now :
                                   bht != timestamp_nonexistent ? bht :
                                   bst != timestamp_nonexistent ? bst :
                                                                  b->timestamp);

                    d = make_shared<build_delay> (move (id.package.tenant),
                                                  move (id.package.name),
                                                  p->version,
                                                  move (id.target),
                                                  move (id.target_config_name),
                                                  move (id.package_config_name),
                                                  move (id.toolchain_name),
                                                  t.second,
                                                  pts);
                    db.persist (d);
                  }

                  // Handle package builds differently based on their tenant's
                  // archive status.
                  //
                  // If the package is not archived then consider it as
                  // delayed if it is not (re-)built by the expiration
                  // time. Otherwise, consider it as delayed if it is unbuilt.
                  //
                  // We also don't need to report an unbuilt archived package
                  // twice, as both soft and hard build delays.
                  //
                  bool hard_delayed;
                  bool soft_delayed;

                  if (!bp.archived)
                  {
                    auto delayed = [&d] (timestamp bt, timestamp be)
                    {
                      timestamp t (bt != timestamp_nonexistent
                                   ? bt
                                   : d->package_timestamp);
                      return t <= be;
                    };

                    hard_delayed = delayed (bht, hard_rebuild_expiration);
                    soft_delayed = delayed (bst, soft_rebuild_expiration);
                  }
                  else
                  {
                    hard_delayed = (bst == timestamp_nonexistent);
                    soft_delayed = false;
                  }

                  // Add hard/soft delays to the respective reports and
                  // collect the delay for update, if it is reported.
                  //
                  // Note that we update the delay objects persistent state
                  // later, after we successfully print the reports.
                  //
                  bool reported (false);

                  if (hard_delayed)
                  {
                    // If the report timeout is zero then report the delay
                    // unconditionally. Otherwise, report the active package
                    // build delay if the report timeout is expired and the
                    // archived package build delay if it was never reported.
                    // Note that fixing the building infrastructure won't help
                    // building an archived package, so reporting its build
                    // delays repeatedly is meaningless.
                    //
                    bool report (
                      ops.report_timeout () == 0 ||
                      (!bp.archived
                       ? d->report_hard_timestamp <= report_expiration
                       : d->report_hard_timestamp == timestamp_nonexistent));

                    if (report)
                    {
                      d->report_hard_timestamp = now;
                      reported = true;
                    }

                    hard_delays_report.add_delay (d, report);
                  }

                  if (soft_delayed)
                  {
                    bool report (ops.report_timeout () == 0 ||
                                 d->report_soft_timestamp <= report_expiration);

                    if (report)
                    {
                      d->report_soft_timestamp = now;
                      reported = true;
                    }

                    soft_delays_report.add_delay (d, report);
                  }

                  // If we don't consider the report timestamps for reporting
                  // delays, it seems natural not to update these timestamps
                  // either. Note that reporting all delays and still updating
                  // the report timestamps can be achieved by specifying the
                  // zero report timeout.
                  //
                  if (reported && ops.report_timeout_specified ())
                    update_delays.insert (move (d));
                }
              }
            }
          }
        }

        t.commit ();
      }
    }

    // Print delay reports, if not empty.
    //
    if (!hard_delays_report.empty () || !soft_delays_report.empty ())
    try
    {
      cerr.exceptions (ostream::badbit | ostream::failbit);

      // Don't print the total delay count if the report timeout is zero since
      // all delays are reported in this case.
      //
      bool total (ops.report_timeout () != 0);

      hard_delays_report.print ("Package hard rebuild delays",
                                total,
                                ops.full_report ());

      // Separate reports with an empty line.
      //
      if (!hard_delays_report.empty () && !soft_delays_report.empty ())
        cerr << endl;

      soft_delays_report.print ("Package soft rebuild delays",
                                total,
                                ops.full_report ());
    }
    catch (const io_error&)
    {
      return 1; // Not much we can do on stderr writing failure.
    }

    // Persist the delay report timestamps.
    //
    if (!update_delays.empty ())
    {
      transaction t (db.begin ());

      for (shared_ptr<const build_delay> d: update_delays)
        db.update (d);

      t.commit ();
    }

    return 0;
  }
  catch (const database_locked&)
  {
    cerr << "brep-monitor or some other brep utility is running" << endl;
    return 2;
  }
  catch (const recoverable& e)
  {
    cerr << "recoverable database error: " << e << endl;
    return 3;
  }
  catch (const cli::exception& e)
  {
    cerr << "error: " << e << endl << help_info << endl;
    return 1;
  }
  catch (const failed&)
  {
    return 1; // Diagnostics has already been issued.
  }
  // Fully qualified to avoid ambiguity with odb exception.
  //
  catch (const std::exception& e)
  {
    cerr << "error: " << e << endl;
    return 1;
  }
}

int
main (int argc, char* argv[])
{
  return brep::main (argc, argv);
}
