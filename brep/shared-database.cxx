// file      : brep/shared-database.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/shared-database>

#include <stdexcept> // runtime_error

#include <odb/pgsql/database.hxx>

#include <brep/types>
#include <brep/utility>

namespace brep
{
  shared_ptr<odb::database>
  shared_database (const string& h, unsigned int p)
  {
    using odb::pgsql::database;
    static weak_ptr<database> db;

    // In C++11, function-static variable initialization is
    // guaranteed to be thread-safe, thought this doesn't
    // seem to be enough in our case (because we are re-
    // initializing the weak pointer).
    //
    if (shared_ptr<database> d = db.lock ())
    {
      if (h != d->host () || p != d->port ())
        throw std::runtime_error ("shared database host/port mismatch");

      return d;
    }
    else
    {
      d = make_shared<database> ("", "", "brep", h, p);
      db = d;
      return d;
    }
  }
}
