// file      : migrate/migrate.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <strings.h> // strcasecmp()

#include <sstream>
#include <iostream>

#include <odb/database.hxx>
#include <odb/transaction.hxx>
#include <odb/schema-catalog.hxx>

#include <odb/pgsql/database.hxx>

#include <libbutl/pager.mxx>

#include <libbrep/package.hxx>
#include <libbrep/package-odb.hxx>
#include <libbrep/database-lock.hxx>

#include <migrate/migrate-options.hxx>

using namespace std;
using namespace odb::core;
using namespace brep;

// Operation failed, diagnostics has already been issued.
//
struct failed {};

static const char* help_info (
  "  info: run 'brep-migrate --help' for more information");

// Helper class that encapsulates both the ODB-generated schema and the
// extra that comes from a .sql file (via xxd).
//
class schema
{
public:
  explicit
  schema (const char* extra, string name);

  void
  create (database&, bool extra_only = false) const;

  void
  drop (database&, bool extra_only = false) const;

private:
  string name_;
  strings drop_statements_;
  strings create_statements_;
};

schema::
schema (const char* s, string name)
    : name_ (move (name))
{
  // Remove comments, saving the cleaned SQL code into statements.
  //
  string statements;
  for (istringstream i (s); i; )
  {
    // Skip leading spaces (including consequtive newlines). In case we
    // hit eof, keep c set to '\n'.
    //
    char c;
    static const string spaces (" \t\n\r");
    for (c = '\n'; i.get (c) && spaces.find (c) != string::npos; c = '\n')
      ;

    // First non-space character (or '\n' for eof). See if this is a comment.
    //
    bool skip (c == '\n' || (c == '-' && i.peek () == '-'));

    // Read until newline (and don't forget the character we already have).
    //
    do
    {
      if (!skip)
        statements.push_back (c);

    } while (c != '\n' && i.get (c));
  }

  istringstream i (move (statements));

  // Build CREATE and DROP statement lists.
  //
  while (i)
  {
    string op;
    if (i >> op) // Get the first word.
    {
      string statement (op);

      auto read_until = [&i, &statement] (const char stop[2]) -> bool
      {
        for (char prev ('\0'), c; i.get (c); prev = c)
        {
          statement.push_back (c);

          if (stop[0] == prev && stop[1] == c)
            return true;
        }

        return false;
      };

      if (strcasecmp (op.c_str (), "CREATE") == 0)
      {
        bool valid (true);

        string kw;
        i >> kw;
        statement += " " + kw;

        if (strcasecmp (kw.c_str (), "FUNCTION") == 0)
        {
          if (!read_until ("$$") || !read_until ("$$"))
          {
            cerr << "error: function body must be defined using $$-quoted "
              "strings" << endl;
            throw failed ();
          }
        }
        else if (strcasecmp (kw.c_str (), "TYPE") == 0)
        {
          // Fall through.
        }
        else if (strcasecmp (kw.c_str (), "FOREIGN") == 0)
        {
          i >> kw;
          statement += " " + kw;
          valid = strcasecmp (kw.c_str (), "TABLE") == 0;

          // Fall through.
        }
        else
          valid = false;

        if (!valid)
        {
          cerr << "error: unexpected CREATE statement" << endl;
          throw failed ();
        }

        if (!read_until (";\n"))
        {
          cerr << "error: expected ';\\n' at the end of CREATE statement"
               << endl;
          throw failed ();
        }

        assert (!statement.empty ());
        create_statements_.emplace_back (move (statement));
      }
      else if (strcasecmp (op.c_str (), "DROP") == 0)
      {
        if (!read_until (";\n"))
        {
          cerr << "error: expected ';\\n' at the end of DROP statement"
               << endl;
          throw failed ();
        }

        assert (!statement.empty ());
        drop_statements_.emplace_back (move (statement));
      }
      else
      {
        cerr << "error: unexpected statement starting with '" << op << "'"
             << endl;
        throw failed ();
      }
    }
  }
}

void schema::
drop (database& db, bool extra_only) const
{
  for (const auto& s: drop_statements_)
    // If the statement execution fails, the corresponding source file line
    // number is not reported. The line number could be usefull for the
    // utility implementer only. The errors seen by the end-user should not be
    // statement-specific.
    //
    db.execute (s);

  if (!extra_only)
    schema_catalog::drop_schema (db, name_);
}

void schema::
create (database& db, bool extra_only) const
{
  drop (db, extra_only);

  if (!extra_only)
    schema_catalog::create_schema (db, name_);

  for (const auto& s: create_statements_)
    db.execute (s);
}

// Register the data migration functions for the package database schema.
//
template <schema_version v>
using package_migration_entry_base =
  data_migration_entry<v, LIBBREP_PACKAGE_SCHEMA_VERSION_BASE>;

template <schema_version v>
struct package_migration_entry: package_migration_entry_base<v>
{
  package_migration_entry (void (*f) (database& db))
      : package_migration_entry_base<v> (f, "package") {}
};

// Set the unbuildable reason for unbuildable packages.
//
// Note that we are unable to restore the exact reason and so always set it
// to 'unbuildable'.
//
// Also note that we don't set the buildable flag to false for the separate
// test packages here. Implementing this properly in the data migration feels
// hairy (see load/load.cxx for details). Instead we rely on brep-load to
// handle this on the next tenant reload that can be enforced by using the
// --force option.
//
//#if 0
static const package_migration_entry<18>
package_migrate_v18 ([] (database& db)
{
  db.execute ("UPDATE package SET unbuildable_reason = 'unbuildable' "
              "WHERE NOT buildable");
});
//#endif

// Merging the package examples and benchmarks tables into the package tests
// table is a bit hairy. Thus, we won't bother with that and just cleanup the
// amended package tests table, relying on the loader to fill it in a short
// time.
//
static const package_migration_entry<19>
package_migrate_v19 ([] (database& db)
{
  db.execute ("DELETE from package_tests");
});

// main() function
//
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
    cout << "brep-migrate " << BREP_VERSION_ID << endl
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
    butl::pager p ("brep-migrate help",
                   false,
                   ops.pager_specified () ? &ops.pager () : nullptr,
                   &ops.pager_option ());

    print_usage (p.stream ());

    // If the pager failed, assume it has issued some diagnostics.
    //
    return p.wait () ? 0 : 1;
  }

  if (!scan.more ())
  {
    cerr << "error: no database schema specified" << endl
         << help_info << endl;
    return 1;
  }

  const string db_schema (scan.next ());

  if (db_schema != "package" && db_schema != "build")
    throw cli::unknown_argument (db_schema);

  if (scan.more ())
  {
    cerr << "error: unexpected argument encountered" << endl
         << help_info << endl;
    return 1;
  }

  if (ops.recreate () && ops.drop ())
  {
    cerr << "error: inconsistent options specified" << endl
         << help_info << endl;
    return 1;
  }

  odb::pgsql::database db (
    ops.db_user (),
    ops.db_password (),
    !ops.db_name ().empty ()
    ? ops.db_name ()
    : "brep_" + db_schema,
    ops.db_host (),
    ops.db_port (),
    "options='-c default_transaction_isolation=serializable'");

  // Prevent several brep utility instances from updating the database
  // simultaneously.
  //
  database_lock l (db);

  // Currently we don't support data migration for the manual database schema
  // migration.
  //
  if (db.schema_migration (db_schema))
  {
    cerr << "error: manual database schema migration is not supported" << endl;
    throw failed ();
  }

  // Need to obtain schema version out of the transaction. If the
  // schema_version table does not exist, the SQL query fails, which makes the
  // transaction useless as all consequitive queries in that transaction will
  // be ignored by PostgreSQL.
  //
  schema_version schema_version (db.schema_version (db_schema));

  odb::schema_version schema_current_version (
    schema_catalog::current_version (db, db_schema));

  // It is impossible to operate with the database which is out of the
  // [base_version, current_version] range due to the lack of the knowlege
  // required not just for migration, but for the database wiping as well.
  //
  if (schema_version > 0)
  {
    if (schema_version < schema_catalog::base_version (db, db_schema))
    {
      cerr << "error: database schema is too old" << endl;
      throw failed ();
    }

    if (schema_version > schema_current_version)
    {
      cerr << "error: database schema is too new" << endl;
      throw failed ();
    }
  }

  bool drop (ops.drop ());
  bool create (ops.recreate () || (schema_version == 0 && !drop));
  assert (!create || !drop);

  // The database schema recreation requires dropping it initially, which is
  // impossible before the database is migrated to the current schema version.
  // Let the user decide if they want to migrate or just drop the entire
  // database (followed with the database creation for the --recreate option).
  //
  if ((create || drop)    &&
      schema_version != 0 &&
      schema_version != schema_current_version)
  {
    cerr << "error: database schema requires migration" << endl
         << "  info: either migrate the database first or drop the entire "
            "database using, for example, psql" << endl;
    throw failed ();
  }

  static const char package_extras[] = {
#include <libbrep/package-extra.hxx>
    , '\0'};

  static const char build_extras[] = {
#include <libbrep/build-extra.hxx>
    , '\0'};

  schema s (db_schema == "package" ? package_extras : build_extras,
            db_schema);

  transaction t (db.begin ());

  if (create || drop)
  {
    if (create)
      s.create (db);
    else if (drop)
      s.drop (db);
  }
  else if (schema_version != schema_current_version)
  {
    // Drop the extras, migrate the database tables and data, and create the
    // extras afterwards.
    //
    // Note that here we assume that the latest extras drop SQL statements can
    // handle entities created by the create statements of the earlier schemas
    // (see libbrep/package-extra.sql for details).
    //
    s.drop (db, true /* extra_only */);

    schema_catalog::migrate (db, 0, db_schema);

    s.create (db, true /* extra_only */);
  }

  t.commit ();
  return 0;
}
catch (const database_locked&)
{
  cerr << "brep-migrate or brep-load is running" << endl;
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
