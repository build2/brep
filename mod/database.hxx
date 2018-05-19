// file      : mod/database.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_DATABASE_HXX
#define MOD_DATABASE_HXX

#include <odb/forward.hxx> // database

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

namespace brep
{
  // Return pointer to the shared database instance, creating one on the first
  // call. Throw odb::exception on failure. Is not thread-safe.
  //
  shared_ptr<odb::core::database>
  shared_database (string user,
                   string role,
                   string password,
                   string name,
                   string host,
                   uint16_t port,
                   size_t max_connections);
}

#endif // MOD_DATABASE_HXX
