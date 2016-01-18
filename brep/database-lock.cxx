// file      : brep/database-lock.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/database-lock>

#include <odb/pgsql/database.hxx>
#include <odb/pgsql/exceptions.hxx>
#include <odb/pgsql/transaction.hxx>

namespace brep
{
  using namespace odb::pgsql;

  database_lock::
  database_lock (database& db)
  {
    // Before locking the table make sure it exists.
    //
    {
      transaction t (db.begin ());
      db.execute ("CREATE TABLE IF NOT EXISTS database_mutex ()");
      t.commit ();
    }

    connection_ = db.connection ();

    // Don't make current. Will be rolled back in destructor.
    //
    transaction_.reset (new transaction (connection_->begin (), false));

    try
    {
      connection_->execute ("LOCK TABLE database_mutex NOWAIT");
    }
    catch (const database_exception& e)
    {
      if (e.sqlstate () == "55P03") // The table is already locked.
        throw database_locked ();

      throw;
    }
  }
}
