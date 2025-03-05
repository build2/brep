// file      : mod/database-module.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_DATABASE_MODULE_HXX
#define MOD_DATABASE_MODULE_HXX

#include <odb/forward.hxx> // odb::core::database, odb::core::connection_ptr

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/module.hxx>
#include <mod/module-options.hxx>
#include <mod/tenant-service.hxx> // tenant_service_map

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
    // above steps on the recoverable database failures (deadlocks, etc). If
    // no more retries left, then cancel the tenant (by calling
    // cancel_tenant()) and throw server_error.
    //
    // Note that the passed connection argument may refer to a different
    // connection object on return. Also note that on the server_error
    // exception the connection is released.
    //
    optional<string>
    update_tenant_service_state (
      odb::core::connection_ptr&,
      const tenant_service_map&,
      const string& type,
      const string& id,
      const function<optional<string> (const string& tenant_id,
                                       const tenant_service&)>&);

    // A low-level version of the above.
    //
    // Specifically, the specified function is expected to change the
    // tenant-associated service state directly and return true if any changes
    // has been made. While at it, it may also change some other tenant
    // members. If it returns true, then the update_tenant_service_state()
    // function assumes that the service state (in a broad sense) was changed,
    // updates the tenant in the database, and returns the tenant service
    // state data.
    //
    // Note that if no tenant with the specified service type/id exists in the
    // database, then the specified function will be called with the NULL
    // pointer.
    //
    optional<string>
    update_tenant_service_state (
      odb::core::connection_ptr&,
      const tenant_service_map&,
      const string& type,
      const string& id,
      const function<bool (const shared_ptr<build_tenant>&)>&);

  public:
    // Cancel a tenant due to the inability to save the associated service
    // data (for example, due to persistent transaction rollbacks). The passed
    // tenant_service argument contains the unsaved service data.
    //
    // Specifically, this function archives the tenant and calls the build
    // canceled service notification.
    //
    // Note that it doesn't clear the tenant service state, which allows the
    // service to still handle requests, if desired. Also note that brep won't
    // call any notifications anymore for this tenant since it is archived
    // now.
    //
    // Repeat the attempts on the recoverable database failures (deadlocks,
    // etc) and throw runtime_error if no more retries left.
    //
    static void
    cancel_tenant (odb::core::connection_ptr&&,
                   size_t retry_max,
                   const tenant_service_map&,
                   const diag_epilogue& log_writer,
                   const string& tenant_id,
                   const tenant_service&);

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
