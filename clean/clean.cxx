// file      : clean/clean.cxx -*- C++ -*-
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
#include <libbrep/build-odb.hxx>
#include <libbrep/package.hxx>
#include <libbrep/package-odb.hxx>
#include <libbrep/build-package.hxx>
#include <libbrep/build-package-odb.hxx>
#include <libbrep/database-lock.hxx>

#include <mod/build-target-config.hxx>

#include <clean/clean-options.hxx>

using namespace std;
using namespace odb::core;

namespace brep
{
  // Operation failed, diagnostics has already been issued.
  //
  struct failed {};

  static const char* help_info (
    "  info: run 'brep-clean --help' for more information");

  static int
  clean_builds (const options&, cli::argv_scanner&, odb::pgsql::database&);

  static int
  clean_tenants (const options&, cli::argv_scanner&, odb::pgsql::database&);

  static int
  main (int argc, char* argv[])
  try
  {
    cli::argv_scanner scan (argc, argv, true);
    options ops (scan);

    // Version.
    //
    if (ops.version ())
    {
      cout << "brep-clean " << BREP_VERSION_ID << endl
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
      butl::pager p ("brep-clean help",
                     false,
                     ops.pager_specified () ? &ops.pager () : nullptr,
                     &ops.pager_option ());

      print_usage (p.stream ());

      // If the pager failed, assume it has issued some diagnostics.
      //
      return p.wait () ? 0 : 1;
    }

    // Detect the mode.
    //
    if (!scan.more ())
    {
      cerr << "error: 'builds' or 'tenants' is expected" << endl
           << help_info << endl;
      return 1;
    }

    const string mode (scan.next ());

    if (mode != "builds" && mode != "tenants")
      throw cli::unknown_argument (mode);

    const string db_schema (mode == "builds" ? "build" : "package");

    const string& db_name (!ops.db_name ().empty ()
                           ? ops.db_name ()
                           : "brep_" + db_schema);

    odb::pgsql::database db (
      ops.db_user (),
      ops.db_password (),
      db_name,
      ops.db_host (),
      ops.db_port (),
      "options='-c default_transaction_isolation=serializable'");

    // Prevent several brep utility instances from updating the database
    // simultaneously.
    //
    database_lock l (db);

    // Check that the database schema matches the current one.
    //
    if (schema_catalog::current_version (db, db_schema) !=
        db.schema_version (db_schema))
    {
      cerr << "error: " << db_schema << " database schema differs from the "
           << "current one" << endl
           << "  info: use brep-migrate to migrate the database" << endl;
      return 1;
    }

    return mode == "builds"
      ? clean_builds  (ops, scan, db)
      : clean_tenants (ops, scan, db);
  }
  catch (const database_locked&)
  {
    cerr << "brep-clean or brep-migrate is running" << endl;
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

  // Convert timeout duration into the time point. Return
  // timestamp_nonexistent (never expire) for zero argument. Return nullopt if
  // the argument is invalid.
  //
  static optional<timestamp>
  timeout (const string& tm)
  {
    char* e (nullptr);
    uint64_t t (strtoull (tm.c_str (), &e, 10));

    if (*e != '\0' || tm.empty ())
      return nullopt;

    if (t == 0)
      return timestamp_nonexistent;

    return system_clock::now () - chrono::hours (t);
  }

  static int
  clean_builds (const options&,
                cli::argv_scanner& scan,
                odb::pgsql::database& db)
  {
    // Load configurations names.
    //
    if (!scan.more ())
    {
      cerr << "error: configuration file expected" << endl
           << help_info << endl;
      return 1;
    }

    path cp;

    try
    {
      cp = path (scan.next ());
    }
    catch (const invalid_path& e)
    {
      cerr << "error: configuration file expected instead of '" << e.path
           << "'" << endl
           << help_info << endl;
      return 1;
    }

    // Load build target configurations.
    //
    build_target_configs configs;

    try
    {
      configs = bbot::parse_buildtab (cp);
    }
    catch (const io_error& e)
    {
      cerr << "error: unable to read '" << cp << "': " << e << endl;
      return 1;
    }

    // Note: contains shallow references to the configuration targets/names.
    //
    set<build_target_config_id> configs_set;

    for (const build_target_config& c: configs)
      configs_set.insert (build_target_config_id {c.target, c.name});

    // Parse timestamps.
    //
    map<string, timestamp> timeouts; // Toolchain timeouts.
    timestamp default_timeout;       // timestamp_nonexistent

    while (scan.more ())
    {
      string a (scan.next ());

      string tc;
      optional<timestamp> to;

      size_t p (a.find ('='));

      if (p == string::npos)
        to = timeout (a);
      else if (p > 0)        // Note: toolchain name can't be empty.
      {
        tc = string (a, 0, p);
        to = timeout (string (a, p + 1));
      }

      // Note that the default timeout can't be zero.
      //
      if (!to || (*to == timestamp_nonexistent && tc.empty ()))
      {
        cerr << "error: timeout expected instead of '" << a << "'" << endl
             << help_info << endl;
        return 1;
      }

      if (tc.empty ())
        default_timeout = *to;

      timeouts[move (tc)] = move (*to);
    }

    // Prepare the build prepared query.
    //
    // Query package builds in chunks in order not to hold locks for too long.
    // Sort the result by package version to minimize number of queries to the
    // package database. Note that we still need to sort by configuration and
    // toolchain to make sure that builds are sorted consistently across
    // queries and we don't miss any of them.
    //
    using bld_query = query<build>;
    using prep_bld_query = prepared_query<build>;

    size_t offset (0);
    bld_query bq ("ORDER BY" +
                  bld_query::id.package.tenant + ","      +
                  bld_query::id.package.name              +
                  order_by_version_desc (bld_query::id.package.version,
                                         false) + ","     +
                  bld_query::id.target + ","              +
                  bld_query::id.target_config_name + ","  +
                  bld_query::id.package_config_name + "," +
                  bld_query::id.toolchain_name            +
                  order_by_version (bld_query::id.toolchain_version,
                                    false /* first */)    +
                  "OFFSET" + bld_query::_ref (offset) + "LIMIT 2000");

    connection_ptr conn (db.connection ());

    prep_bld_query bld_prep_query (
      conn->prepare_query<build> ("build-query", bq));

    // Prepare the package version query.
    //
    // Query buildable packages every time the new tenant or package name is
    // encountered during iterating over the package builds. Such a query will
    // be made once per tenant package name due to the builds query sorting
    // criteria (see above).
    //
    using pkg_query = query<build_package_version>;
    using prep_pkg_query = prepared_query<build_package_version>;

    string tnt;
    package_name pkg_name;
    set<version> package_versions;

    pkg_query pq (pkg_query::buildable                          &&
                  pkg_query::id.tenant == pkg_query::_ref (tnt) &&
                  pkg_query::id.name == pkg_query::_ref (pkg_name));

    prep_pkg_query pkg_prep_query (
      conn->prepare_query<build_package_version> ("package-query", pq));

    // On the recoverable database error we will retry querying/traversing
    // builds in the current chunk, up to 10 times. If we still end up with
    // the recoverable database error, then just skip this builds chunk.
    //
    const size_t max_retries (10);
    size_t retry (max_retries);

    // If we fail to erase some builds due to the recoverable database error
    // and no builds are erased during this run, then we terminate with the
    // exit code 3 (recoverable database error).
    //
    bool erased (false);
    optional<string> re;

    for (bool ne (true); ne; )
    {
      size_t n (0);

      try
      {
        transaction t (conn->begin ());

        // Query builds.
        //
        auto builds (bld_prep_query.execute ());

        n = builds.size ();

        size_t not_erased (0);

        if ((ne = (n != 0)))
        {
          for (const auto& b: builds)
          {
            auto i (timeouts.find (b.toolchain_name));

            timestamp et (i != timeouts.end ()
                          ? i->second
                          : default_timeout);

            // Note that we don't consider the case when both the
            // configuration and the package still exist but the package now
            // excludes the configuration (configuration is now of the legacy
            // class instead of the default class, etc). Should we handle this
            // case and re-implement in a way brep-monitor does it? Probably
            // not since the described situation is not very common and
            // storing some extra builds which sooner or later will be wiped
            // out due to the timeout is harmless. The current implementation,
            // however, is simpler and consumes less resources in runtime
            // (doesn't load build package objects, etc).
            //
            bool cleanup (
              // Check that the build is not stale.
              //
              b.timestamp <= et ||

              // Check that the build configuration is still present.
              //
              // Note that we unable to detect configuration changes and rely
              // on periodic rebuilds to take care of that.
              //
              configs_set.find (
                build_target_config_id {
                  b.target, b.target_config_name}) == configs_set.end ());

            // Check that the build package still exists.
            //
            if (!cleanup)
            {
              if (tnt != b.tenant || pkg_name != b.package_name)
              {
                tnt = b.tenant;
                pkg_name = b.package_name;
                package_versions.clear ();

                for (auto& p: pkg_prep_query.execute ())
                  package_versions.emplace (move (p.version));
              }

              cleanup = package_versions.find (b.package_version) ==
                        package_versions.end ();
            }

            if (cleanup)
              db.erase (b);
            else
              ++not_erased;
          }
        }

        t.commit ();

        if (!erased)
          erased = (not_erased != n);

        offset += not_erased;
        retry = max_retries;
      }
      catch (const recoverable& e)
      {
        // Re-iterate over the current builds chunk, unless there are no more
        // attempts left. In the later case stash the error message, if not
        // stashed yet, and skip the current builds chunk.
        //
        if (retry-- == 0)
        {
          offset += n;
          retry = max_retries;

          if (!re)
            re = e.what ();
        }

        tnt = "";
        pkg_name = package_name ();
        package_versions.clear ();
      }
    }

    if (re && !erased)
    {
      cerr << "recoverable database error: " << *re << endl;
      return 3;
    }

    return 0;
  }

  static int
  clean_tenants (const options& ops,
                 cli::argv_scanner& scan,
                 odb::pgsql::database& db)
  {
    if (!scan.more ())
    {
      cerr << "error: timeout expected" << endl
           << help_info << endl;
      return 1;
    }

    string a (scan.next ());
    optional<timestamp> to (timeout (a));

    // Note that the timeout can't be zero.
    //
    if (!to || *to == timestamp_nonexistent)
    {
      cerr << "error: timeout expected instead of '" << a << "'" << endl
           << help_info << endl;
      return 1;
    }

    if (scan.more ())
    {
      cerr << "error: unexpected argument encountered" << endl
           << help_info << endl;
      return 1;
    }

    uint64_t ns (
      chrono::duration_cast<chrono::nanoseconds> (
        to->time_since_epoch ()).count ());

    // Query tenants in chunks in order not to hold locks for too long.
    //
    connection_ptr conn (db.connection ());

    // Archive (rather then delete) old tenants, if requested.
    //
    if (ops.archive ())
    {
      using query = query<tenant>;
      using pquery = prepared_query<tenant>;

      query q ((query::creation_timestamp < ns && !query::archived) +
               "LIMIT 100");

      pquery pq (conn->prepare_query<tenant> ("tenant-query", q));

      for (bool ne (true); ne; )
      {
        transaction t (conn->begin ());

        auto tenants (pq.execute ());
        if ((ne = !tenants.empty ()))
        {
          for (auto& t: tenants)
          {
            t.archived = true;
            db.update (t);
          }
        }

        t.commit ();
      }

      return 0;
    }

    // Delete old tenants.
    //
    // Note that we don't delete dangling builds for the deleted packages.
    // Doing so would require to operate on two databases, complicating the
    // code and the utility interface. Note that dangling builds are never
    // considered in the web interface and are always deleted with the
    // 'brep-clean builds' command.
    //
    using query = query<tenant_id>;
    using pquery = prepared_query<tenant_id>;

    query q ((query::creation_timestamp < ns) + "LIMIT 100");
    pquery pq (conn->prepare_query<tenant_id> ("tenant-id-query", q));

    for (bool ne (true); ne; )
    {
      transaction t (conn->begin ());

      auto tenant_ids (pq.execute ());
      if ((ne = !tenant_ids.empty ()))
      {
        // Cache tenant ids and erase packages, repositories, public keys, and
        // tenants at once.
        //
        strings tids;
        tids.reserve (tenant_ids.size ());

        for (auto& tid: tenant_ids)
          tids.push_back (move (tid.value));

        using odb::query;

        db.erase_query<package> (
          query<package>::id.tenant.in_range (tids.begin (), tids.end ()));

        db.erase_query<repository> (
          query<repository>::id.tenant.in_range (tids.begin (), tids.end ()));

        db.erase_query<public_key> (
          query<public_key>::id.tenant.in_range (tids.begin (), tids.end ()));

        db.erase_query<tenant> (
          query<tenant>::id.in_range (tids.begin (), tids.end ()));
      }

      t.commit ();
    }

    return 0;
  }
}

int
main (int argc, char* argv[])
{
  return brep::main (argc, argv);
}
