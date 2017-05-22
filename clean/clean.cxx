// file      : clean/clean.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <set>
#include <iostream>

#include <odb/database.hxx>
#include <odb/transaction.hxx>
#include <odb/schema-catalog.hxx>

#include <odb/pgsql/database.hxx>

#include <libbutl/pager.hxx>

#include <libbbot/build-config.hxx>

#include <libbrep/build.hxx>
#include <libbrep/build-odb.hxx>
#include <libbrep/package.hxx>
#include <libbrep/package-odb.hxx>
#include <libbrep/database-lock.hxx>

#include <clean/clean-options.hxx>

using namespace std;
using namespace bbot;
using namespace brep;
using namespace odb::core;

// Operation failed, diagnostics has already been issued.
//
struct failed {};

static const char* help_info (
  "  info: run 'brep-clean --help' for more information");

int
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
         << "Copyright (c) 2014-2017 Code Synthesis Ltd" << endl
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

  const toolchain_timeouts& timeouts (ops.stale_timeout ());

  auto i (timeouts.find (string ()));
  timestamp default_timeout (i != timeouts.end ()
                             ? i->second
                             : timestamp_nonexistent);

  // Load configurations names.
  //
  if (!scan.more ())
  {
    cerr << "error: configuration file expected" << endl
         << help_info << endl;
    return 1;
  }

  set<string> configs;
  for (auto& c: parse_buildtab (path (scan.next ())))
    configs.emplace (move (c.name));

  if (scan.more ())
  {
    cerr << "error: unexpected argument encountered" << endl
         << help_info << endl;
    return 1;
  }

  odb::pgsql::database build_db (
    ops.build_db_user (),
    ops.build_db_password (),
    ops.build_db_name (),
    ops.build_db_host (),
    ops.build_db_port (),
    "options='-c default_transaction_isolation=serializable'");

  odb::pgsql::database package_db (
    ops.package_db_user (),
    ops.package_db_password (),
    ops.package_db_name (),
    ops.package_db_host (),
    ops.package_db_port (),
    "options='-c default_transaction_isolation=serializable'");

  // Prevent several brep-clean/migrate instances from updating build database
  // simultaneously.
  //
  database_lock l (build_db);

  // Check that the build and package database schemas match the current ones.
  //
  const string bs ("build");
  if (schema_catalog::current_version (build_db, bs) !=
      build_db.schema_version (bs))
  {
    cerr << "error: build database schema differs from the current one"
         << endl << "  info: use brep-migrate to migrate the database" << endl;
    return 1;
  }

  const string ps ("package");
  if (schema_catalog::current_version (package_db, ps) !=
      package_db.schema_version (ps))
  {
    cerr << "error: package database schema differs from the current one"
         << endl << "  info: use brep-migrate to migrate the database" << endl;
    return 1;
  }

  // Prepare the build prepared query.
  //
  // Query package builds in chunks in order not to hold locks for too long.
  // Sort the result by package version to minimize number of queries to the
  // package database.
  //
  using bld_query = query<build>;
  using prep_bld_query = prepared_query<build>;

  size_t offset (0);
  bld_query bq ("ORDER BY" + bld_query::id.package.name +
                order_by_version_desc (bld_query::id.package.version, false) +
                "OFFSET" + bld_query::_ref (offset) + "LIMIT 100");

  connection_ptr bld_conn (build_db.connection ());

  prep_bld_query bld_prep_query (
    bld_conn->prepare_query<build> ("build-query", bq));

  // Prepare the package version query.
  //
  // Query package versions every time the new package name is encountered
  // during iterating over the package builds. Such a query will be made once
  // per package name due to the builds query sorting criteria (see above).
  //
  using pkg_query = query<package_version>;
  using prep_pkg_query = prepared_query<package_version>;

  string package_name;
  set<version> package_versions;

  pkg_query pq (pkg_query::package::id.name == pkg_query::_ref (package_name));

  connection_ptr pkg_conn (package_db.connection ());

  prep_pkg_query pkg_prep_query (
    pkg_conn->prepare_query<package_version> ("package-version-query", pq));

  while (true)
  {
    // Start the build database transaction.
    //
    transaction bt (bld_conn->begin ());

    // Query builds.
    //
    auto builds (bld_prep_query.execute ());

    if (!builds.empty ())
    {
      // Start the package database transaction.
      //
      transaction pt (pkg_conn->begin (), false);

      for (const auto& b: builds)
      {
        auto i (timeouts.find (b.toolchain_name));

        timestamp et (i != timeouts.end ()
                      ? i->second
                      : default_timeout);

        bool cleanup (
          // Check that the build is not stale.
          //
          b.timestamp <= et ||

          // Check that the build configuration is still present.
          //
          // Note that we unable to detect configuration changes and rely on
          // periodic rebuilds to take care of that.
          //
          configs.find (b.configuration) == configs.end ());

        // Check that the build package still exists.
        //
        if (!cleanup)
        {
          if (package_name != b.package_name)
          {
            // Switch to the package database transaction.
            //
            transaction::current (pt);

            package_name = b.package_name;
            package_versions.clear ();

            for (auto& v: pkg_prep_query.execute ())
              package_versions.emplace (move (v.version));

            // Switch back to the build database transaction.
            //
            transaction::current (bt);
          }

          cleanup = package_versions.find (b.package_version) ==
            package_versions.end ();
        }

        if (cleanup)
          build_db.erase (b);
        else
          ++offset;
      }

      // Commit the package database transaction.
      //
      pt.commit ();
    }

    bt.commit ();

    if (builds.empty ())
      break;
  }

  return 0;
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
