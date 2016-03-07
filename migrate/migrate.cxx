// file      : migrate/migrate.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <strings.h> // strcasecmp()

#include <sstream>
#include <iostream>

#include <odb/database.hxx>
#include <odb/transaction.hxx>
#include <odb/schema-catalog.hxx>

#include <odb/pgsql/database.hxx>

#include <butl/pager>

#include <brep/types>
#include <brep/utility>
#include <brep/version>

#include <brep/database-lock>

#include <migrate/migrate-options>

using namespace std;
using namespace odb::core;
using namespace brep;

// Operation failed, diagnostics has already been issued.
//
struct failed: std::exception {};

static const char* help_info (
  "  info: run 'brep-migrate --help' for more information");

// Helper class that encapsulates both the ODB-generated schema and the
// extra that comes from a .sql file (via xxd).
//
class schema
{
public:
  explicit
  schema (const char* extra);

  void
  create (database&) const;

  void
  drop (database&) const;

private:
  strings drop_statements_;
  strings create_statements_;
};

schema::
schema (const char* s)
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

      auto read_until = [&i, &statement](const char stop[2]) -> bool
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
        else
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
drop (database& db) const
{
  for (const auto& s: drop_statements_)
    // If the statement execution fails, the corresponding source file line
    // number is not reported. The line number could be usefull for the utility
    // implementer only. The errors seen by the end-user should not be
    // statement-specific.
    //
    db.execute (s);

  schema_catalog::drop_schema (db);
}

void schema::
create (database& db) const
{
  drop (db);

  schema_catalog::create_schema (db);

  for (const auto& s: create_statements_)
    db.execute (s);
}

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
    cout << "brep-migrate " << BREP_VERSION_STR << endl
         << "libbrep " << LIBBREP_VERSION_STR << endl
         << "libbpkg " << LIBBPKG_VERSION_STR << endl
         << "libbutl " << LIBBUTL_VERSION_STR << endl
         << "Copyright (c) 2014-2016 Code Synthesis Ltd" << endl
         << "MIT; see accompanying LICENSE file" << endl;

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
    return p.wait () ? 0 : 2;
  }

  if (argc > 1)
  {
    cerr << "error: unexpected argument encountered" << endl
         << help_info << endl;
    return 2;
  }

  if (ops.recreate () && ops.drop ())
  {
    cerr << "error: inconsistent options specified" << endl
         << help_info << endl;
    return 2;
  }

  odb::pgsql::database db (ops.db_user (),
                           ops.db_password (),
                           ops.db_name (),
                           ops.db_host (),
                           ops.db_port ());

  // Prevent several brep-migrate/load instances from updating DB
  // simultaneously.
  //
  database_lock l (db);

  // Need to obtain schema version out of the transaction. If the
  // schema_version table does not exist, the SQL query fails, which makes the
  // transaction useless as all consequitive queries in that transaction will
  // be ignored by PostgreSQL.
  //
  schema_version schema_version (db.schema_version ());

  // It is impossible to operate with the database which is out of the
  // [base_version, current_version] range due to the lack of the knowlege
  // required not just for migration, but for the database wiping as well.
  //
  if (schema_version > 0)
  {
    if (schema_version < schema_catalog::base_version (db))
    {
      cerr << "error: database schema is too old" << endl;
      throw failed ();
    }

    if (schema_version > schema_catalog::current_version (db))
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
  if ((create || drop) && schema_version != 0 &&
      schema_version != schema_catalog::current_version (db))
  {
    cerr << "error: database schema requires migration" << endl
         << "  info: either migrate the database first or drop the entire "
            "database using, for example, psql" << endl;
    throw failed ();
  }

  transaction t (db.begin ());

  if (create || drop)
  {
    static const char extras[] = {
#include <brep/package-extra>
      , '\0'};

    schema s (extras);

    if (create)
      s.create (db);
    else if (drop)
      s.drop (db);
  }
  else
  {
    // Register the data migration functions.
    //
    // static const data_migration_entry<2, LIBBREP_SCHEMA_VERSION_BASE>
    // migrate_v2_entry (&migrate_v2);
    //
    schema_catalog::migrate (db);
  }

  t.commit ();
  return 0;
}
catch (const database_locked&)
{
  cerr << "brep-migrate or brep-load instance is running" << endl;
  return 1;
}
catch (const cli::exception& e)
{
  cerr << "error: " << e << endl << help_info << endl;
  return 2;
}
catch (const failed&)
{
  return 2; // Diagnostics has already been issued.
}
// Fully qualified to avoid ambiguity with odb exception.
//
catch (const std::exception& e)
{
  cerr << "error: " << e.what () << endl;
  return 2;
}
