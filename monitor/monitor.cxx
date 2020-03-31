// file      : monitor/monitor.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <map>
#include <set>
#include <chrono>
#include <iostream>
#include <algorithm> // find_if()

#include <odb/database.hxx>
#include <odb/transaction.hxx>
#include <odb/schema-catalog.hxx>

#include <odb/pgsql/database.hxx>

#include <libbutl/pager.mxx>
#include <libbutl/utility.mxx> // compare_c_string

#include <libbbot/build-config.hxx>

#include <libbrep/build.hxx>
#include <libbrep/common.hxx>
#include <libbrep/build-odb.hxx>
#include <libbrep/build-package.hxx>
#include <libbrep/build-package-odb.hxx>
#include <libbrep/database-lock.hxx>

#include <mod/build-config.hxx>

#include <monitor/module-options.hxx>
#include <monitor/monitor-options.hxx>

using namespace std;
using namespace butl;
using namespace bbot;
using namespace odb::core;

namespace brep
{
  // Operation failed, diagnostics has already been issued.
  //
  struct failed {};

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
    }

    if (!mod_ops.build_config_specified ())
    {
      cerr << "warning: package building functionality is disabled" << endl;
      return 0;
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
    build_configs configs;

    try
    {
      configs = parse_buildtab (mod_ops.build_config ());
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
      using config_map = map<const char*,
                             const build_config*,
                             compare_c_string>;

      config_map conf_map;
      for (const build_config& c: configs)
        conf_map[c.name.c_str ()] = &c;

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
               query::id.package.tenant + "," +
               query::id.package.name +
               order_by_version (query::id.package.version,
                                 false /* first */) + "," +
               query::id.configuration + "," +
               query::id.toolchain_name +
               order_by_version (query::id.toolchain_version,
                                 false /* first */) +
               "OFFSET" + query::_ref (offset) + "LIMIT 100");

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
              (ci = conf_map.find (d.configuration.c_str ())) ==
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

              cleanup = (p == nullptr  ||
                         !p->buildable ||
                         exclude (p->builds,
                                  p->constraints,
                                  *ci->second,
                                  configs.class_inheritance_map));
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

    // Collect and report delays as separate steps not to hold database locks
    // while printing to stderr. Also we need to properly order delays for
    // printing.
    //
    // Iterate through all possible package builds creating the list of delays
    // with the following sort priority:
    //
    // 1: toolchain name
    // 2: toolchain version (descending)
    // 3: configuration name
    // 4: tenant
    // 5: package name
    // 6: package version (descending)
    //
    // Such ordering will allow us to group build delays by toolchain and
    // configuration while printing the report.
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

        if (int r = x->configuration.compare (y->configuration))
          return r < 0;

        if (int r = x->tenant.compare (y->tenant))
          return r < 0;

        if (int r = x->package_name.compare (y->package_name))
          return r < 0;

        return x->package_version.compare (y->package_version) > 0;
      }
    };

    size_t reported_delay_count (0);
    size_t total_delay_count (0);

    set<shared_ptr<const build_delay>, compare_delay> delays;
    {
      connection_ptr conn (db.connection ());

      // Prepare the buildable package prepared query.
      //
      // Query buildable packages in chunks in order not to hold locks for
      // too long.
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

      // Prepare the package build prepared query.
      //
      // This query will only be used for toolchains that have no version
      // specified on the command line to obtain the latest completed build
      // across all toolchain versions, if present, and the latest incomplete
      // build otherwise.
      //
      using bquery = query<package_build>;
      using prep_bquery = prepared_query<package_build>;

      build_id id;
      const auto& bid (bquery::build::id);

      bquery bq ((equal<package_build> (bid.package, id.package)       &&
                  bid.configuration == bquery::_ref (id.configuration) &&
                  bid.toolchain_name == bquery::_ref (id.toolchain_name)) +
                 "ORDER BY"                                               +
                 bquery::build::completion_timestamp + "DESC, "           +
                 bquery::build::timestamp + "DESC"                        +
                 "LIMIT 1");

      prep_bquery pbq (
        conn->prepare_query<package_build> ("package-build-query", bq));

      timestamp::duration build_timeout (
        ops.build_timeout_specified ()
        ? chrono::seconds (ops.build_timeout ())
        : chrono::seconds (mod_ops.build_normal_rebuild_timeout () +
                           mod_ops.build_result_timeout ()));

      timestamp now (system_clock::now ());

      timestamp build_expiration (now - build_timeout);

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
            shared_ptr<build_package> p (db.load<build_package> (bp.id));

            for (const build_config& c: configs)
            {
              if (exclude (p->builds,
                           p->constraints,
                           c,
                           configs.class_inheritance_map))
                continue;

              for (const pair<string, version>& t: toolchains)
              {
                id = build_id (p->id, c.name, t.first, t.second);

                // If the toolchain version is unspecified then search for the
                // latest build across all toolchain versions and search for a
                // specific build otherwise.
                //
                shared_ptr<build> b;

                if (id.toolchain_version.empty ())
                {
                  auto pbs (pbq.execute ());

                  if (!pbs.empty ())
                    b = move (pbs.begin ()->build);
                }
                else
                  b = db.find<build> (id);

                // Note that we consider a build as delayed if it is not
                // completed in the expected timeframe. So even if the build
                // task have been issued recently we may still consider the
                // build as delayed.
                //
                timestamp bct (b != nullptr
                               ? b->completion_timestamp
                               : timestamp_nonexistent);

                // Create the delay object to record a timestamp when the
                // package build could have potentially been started, unless
                // it already exists.
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

                  // Use the build completion or build status change
                  // timestamp, whichever is earlier, as the build delay
                  // tracking starting point and fallback to the current time
                  // if there is no build yet.
                  //
                  timestamp pts (
                    b == nullptr                                       ? now :
                    bct != timestamp_nonexistent && bct < b->timestamp ? bct :
                    b->timestamp);

                  d = make_shared<build_delay> (move (id.package.tenant),
                                                move (id.package.name),
                                                p->version,
                                                move (id.configuration),
                                                move (id.toolchain_name),
                                                t.second,
                                                pts);
                  db.persist (d);
                }

                // Handle package builds differently based on their tenant's
                // archive status.
                //
                // If the package is not archived then consider it as delayed
                // if it is not (re-)built by the expiration time. Otherwise,
                // consider it as delayed if it is unbuilt.
                //
                bool delayed;

                if (!bp.archived)
                {
                  timestamp bts (bct != timestamp_nonexistent
                                 ? bct
                                 : d->package_timestamp);

                  delayed = (bts <= build_expiration);
                }
                else
                  delayed = (bct == timestamp_nonexistent);

                if (delayed)
                {
                  // If the report timeout is zero then report the delay
                  // unconditionally. Otherwise, report the active package
                  // build delay if the report timeout is expired and the
                  // archived package build delay if it was never reported.
                  // Note that fixing the building infrastructure won't help
                  // building an archived package, so reporting its build
                  // delays repeatedly is meaningless.
                  //
                  if (ops.report_timeout () == 0 ||
                      (!bp.archived
                       ? d->report_timestamp <= report_expiration
                       : d->report_timestamp == timestamp_nonexistent))
                  {
                    // Note that we update the delay objects persistent state
                    // later, after we successfully print the report.
                    //
                    d->report_timestamp = now;
                    delays.insert (move (d));

                    ++reported_delay_count;
                  }
                  //
                  // In the brief mode also collect unreported delays to
                  // deduce and print the total number of delays per
                  // configuration. Mark such delays with the
                  // timestamp_nonexistent report timestamp.
                  //
                  else if (!ops.full_report ())
                  {
                    d->report_timestamp = timestamp_nonexistent;
                    delays.insert (move (d));
                  }

                  ++total_delay_count;
                }
              }
            }
          }
        }

        t.commit ();
      }
    }

    // Report package build delays, if any.
    //
    if (reported_delay_count != 0)
    try
    {
      // Print the report.
      //
      cerr.exceptions (ostream::badbit | ostream::failbit);

      // Don't print the total delay count if the report timeout is zero since
      // all delays are reported in this case.
      //
      bool print_total_delay_count (ops.report_timeout () != 0);

      cerr << "Package build delays (" << reported_delay_count;

      if (print_total_delay_count)
        cerr << '/' << total_delay_count;

      cerr << "):" << endl;

      // Group the printed delays by toolchain and configuration.
      //
      const string*  toolchain_name    (nullptr);
      const version* toolchain_version (nullptr);
      const string*  configuration     (nullptr);

      // In the brief report mode print the number of reported/total delayed
      // package builds per configuration rather than the packages themselves.
      //
      size_t config_reported_delay_count (0);
      size_t config_total_delay_count (0);

      auto brief_config = [&configuration,
                           &config_reported_delay_count,
                           &config_total_delay_count,
                           print_total_delay_count] ()
      {
        if (configuration != nullptr)
        {
          // Only print configurations with delays that needs to be reported.
          //
          if (config_reported_delay_count != 0)
          {
            cerr << "    " << *configuration << " ("
                 << config_reported_delay_count;

            if (print_total_delay_count)
              cerr << '/' << config_total_delay_count;

            cerr << ')' << endl;
          }

          config_reported_delay_count = 0;
          config_total_delay_count = 0;
        }
      };

      for (shared_ptr<const build_delay> d: delays)
      {
        // Print the toolchain, if changed.
        //
        if (toolchain_name == nullptr            ||
            d->toolchain_name != *toolchain_name ||
            d->toolchain_version != *toolchain_version)
        {
          if (!ops.full_report ())
            brief_config ();

          if (toolchain_name != nullptr)
            cerr << endl;

          cerr << "  " << d->toolchain_name;

          if (!d->toolchain_version.empty ())
            cerr << "/" << d->toolchain_version;

          cerr << endl;

          toolchain_name    = &d->toolchain_name;
          toolchain_version = &d->toolchain_version;
          configuration     = nullptr;
        }

        // Print the configuration, if changed.
        //
        if (configuration == nullptr || d->configuration != *configuration)
        {
          if (ops.full_report ())
          {
            if (configuration != nullptr)
              cerr << endl;

            cerr << "    " << d->configuration << endl;
          }
          else
            brief_config ();

          configuration = &d->configuration;
        }

        // Print the delayed build package in the full report mode and count
        // configuration builds otherwise.
        //
        if (ops.full_report ())
        {
          // We can potentially extend this information with the archived flag
          // or the delay duration.
          //
          cerr << "      " << d->package_name << "/" << d->package_version;

          if (!d->tenant.empty ())
            cerr << " " << d->tenant;

          cerr << endl;
        }
        else
        {
          if (d->report_timestamp != timestamp_nonexistent)
            ++config_reported_delay_count;

          ++config_total_delay_count;
        }
      }

      if (!ops.full_report ())
        brief_config ();

      // Persist the delay report timestamps.
      //
      // If we don't consider the report timestamps for reporting delays, it
      // seems natural not to update these timestamps either. Note that
      // reporting all delays and still updating the report timestamps can be
      // achieved by specifying the zero report timeout.
      //
      if (ops.report_timeout_specified ())
      {
        transaction t (db.begin ());

        for (shared_ptr<const build_delay> d: delays)
        {
          // Only update timestamps for delays that needs to be reported.
          //
          if (d->report_timestamp != timestamp_nonexistent)
            db.update (d);
        }

        t.commit ();
      }
    }
    catch (const io_error&)
    {
      return 1; // Not much we can do on stderr writing failure.
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
