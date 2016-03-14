// file      : brep/database.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/database>

#include <map>

#include <odb/pgsql/database.hxx>

namespace brep
{
  namespace options
  {
    bool
    operator< (const db& x, const db& y)
    {
      int r;
      if ((r = x.db_user ().compare (y.db_user ())) != 0 ||
          (r = x.db_password ().compare (y.db_password ())) != 0 ||
          (r = x.db_name ().compare (y.db_name ())) != 0 ||
          (r = x.db_host ().compare (y.db_host ())))
        return r < 0;

      return x.db_port () < y.db_port ();
    }
  }

  shared_ptr<odb::database>
  shared_database (const options::db& o)
  {
    using odb::pgsql::database;

    static std::map<options::db, weak_ptr<database>> databases;

    auto i (databases.find (o));
    if (i != databases.end ())
    {
      if (shared_ptr<database> d = i->second.lock ())
        return d;
    }

    shared_ptr<database> d (
      make_shared<database> (
        o.db_user (),
        o.db_password (),
        o.db_name (),
        o.db_host (),
        o.db_port (),
        "options='-c default_transaction_isolation=serializable'"));

    databases[o] = d;
    return d;
  }
}
