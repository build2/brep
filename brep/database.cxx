// file      : brep/database.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/database>

#include <stdexcept> // runtime_error

#include <odb/pgsql/database.hxx>

#include <brep/types>
#include <brep/utility>

namespace brep
{
  shared_ptr<odb::database>
  shared_database (const options::db& o)
  {
    using odb::pgsql::database;
    static weak_ptr<database> db;

    // In C++11, function-static variable initialization is guaranteed to be
    // thread-safe, thought this doesn't seem to be enough in our case
    // (because we are re-initializing the weak pointer).
    //
    if (shared_ptr<database> d = db.lock ())
    {
      if (o.db_user ()     != d->user ()     ||
          o.db_password () != d->password () ||
          o.db_name ()     != d->db ()       ||
          o.db_host ()     != d->host ()     ||
          o.db_port ()     != d->port ())
        throw std::runtime_error ("shared database options mismatch");

      return d;
    }
    else
    {
      d = make_shared<database> (o.db_user (),
                                 o.db_password (),
                                 o.db_name (),
                                 o.db_host (),
                                 o.db_port ());
      db = d;
      return d;
    }
  }
}
