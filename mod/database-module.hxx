// file      : mod/database-module.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_DATABASE_MODULE_HXX
#define MOD_DATABASE_MODULE_HXX

#include <odb/forward.hxx> // database

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/module.hxx>
#include <mod/options.hxx>

namespace brep
{
  // A handler that utilises the database. Specifically, it will retry the
  // request in the face of recoverable database failures (deadlock, loss of
  // connection, etc) up to a certain number of times.
  //
  class database_module: public handler
  {
  protected:
    database_module () = default;

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    explicit
    database_module (const database_module&);

    // Required to avoid getting warning from clang that
    // database_module::init() hides handler::init() virtual functions. This
    // way all functions get to the same scope and become overloaded set.
    //
    using handler::init;

    // Initialize the package database instance. Throw odb::exception on
    // failure.
    //
    void
    init (const options::package_db&, size_t retry);

    // Initialize the build database instance. Throw odb::exception on
    // database failure.
    //
    void
    init (const options::build_db&, size_t retry);

    virtual bool
    handle (request&, response&) = 0;

  protected:
    size_t retry_ = 0; // Max of all retries.

    shared_ptr<odb::core::database> package_db_;
    shared_ptr<odb::core::database> build_db_;   // NULL if not building.

  private:
    virtual bool
    handle (request&, response&, log&);
  };
}

#endif // MOD_DATABASE_MODULE_HXX
