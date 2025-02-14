// file      : mod/database-module.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_DATABASE_MODULE_HXX
#define MOD_DATABASE_MODULE_HXX

#include <odb/forward.hxx> // odb::core::database, odb::core::connection_ptr

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/module.hxx>
#include <mod/module-options.hxx>

namespace brep
{
  class build_tenant;
  struct tenant_service;

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
    init (const options::package_db&, size_t retry_max);

    // Initialize the build database instance. Throw odb::exception on
    // database failure.
    //
    void
    init (const options::build_db&, size_t retry_max);

    virtual bool
    handle (request&, response&) = 0;

    // Helpers.
    //

    // Update the tenant-associated service state if the specified
    // notification callback-returned function (expected to be not NULL)
    // returns the new state data. Return the service state data, if updated,
    // and nullopt otherwise.
    //
    // Specifically, start the database transaction, query the service state,
    // and, if present, call the callback-returned function on this state. If
    // this call returns the data string (rather than nullopt), then update
    // the service state with this data and persist the change. Repeat all the
    // above steps on the recoverable database failures (deadlocks, etc).
    //
    optional<string>
    update_tenant_service_state (
      const odb::core::connection_ptr&,
      const string& type,
      const string& id,
      const function<optional<string> (const string& tenant_id,
                                       const tenant_service&)>&);

    // A low-level version of the above.
    //
    // Specifically, the specified function is expected to update the
    // tenant-associated service state directly, if required. While at it, it
    // may also update some other tenant members. Note that if no tenant with
    // the specified service type/id exists in the database, then the function
    // will be called with the NULL pointer.
    //
    void
    update_tenant_service_state (
      const odb::core::connection_ptr&,
      const string& type,
      const string& id,
      const function<void (const shared_ptr<build_tenant>&)>&);

  protected:
    size_t retry_     = 0; // Performed retries.
    size_t retry_max_ = 0; // Maximum number of retries to perform.

    shared_ptr<odb::core::database> package_db_;
    shared_ptr<odb::core::database> build_db_;   // NULL if not building.

  private:
    virtual bool
    handle (request&, response&, log&);
  };
}

#endif // MOD_DATABASE_MODULE_HXX
