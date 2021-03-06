// file      : libbrep/database-lock.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBREP_DATABASE_LOCK_HXX
#define LIBBREP_DATABASE_LOCK_HXX

#include <odb/pgsql/forward.hxx>    // database, transaction
#include <odb/pgsql/connection.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

namespace brep
{
  struct database_locked: std::exception
  {
    virtual char const*
    what () const throw () {return "database locked";}
  };

  // Try to "lock" the PostgreSQL database in the constructor and release the
  // lock in the destructor. Throw database_locked if the database is already
  // locked by someone else. May also throw odb::pgsql::database_exception.
  //
  // This mechanism is used by the brep loader and schema migration tool to
  // make sure they don't step on each others toes.
  //
  // Note: movable but not copyable.
  //
  class database_lock
  {
  public:
    explicit
    database_lock (odb::pgsql::database&);

  private:
    odb::pgsql::connection_ptr connection_;
    unique_ptr<odb::pgsql::transaction> transaction_;
  };
}

#endif // LIBBREP_DATABASE_LOCK_HXX
