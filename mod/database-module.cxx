// file      : mod/database-module.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/database-module.hxx>

#include <odb/database.hxx>
#include <odb/exceptions.hxx>
#include <odb/transaction.hxx>

#include <libbrep/build-package.hxx>
#include <libbrep/build-package-odb.hxx>

#include <mod/database.hxx>
#include <mod/module-options.hxx>

namespace brep
{
  using namespace odb::core;

  // While currently the user-defined copy constructor is not required (we
  // don't need to deep copy nullptr's), it is a good idea to keep the
  // placeholder ready for less trivial cases.
  //
  database_module::
  database_module (const database_module& r)
      : handler (r),
        retry_ (r.retry_),
        package_db_ (r.initialized_ ? r.package_db_ : nullptr),
        build_db_ (r.initialized_ ? r.build_db_ : nullptr)
  {
  }

  void database_module::
  init (const options::package_db& o, size_t retry)
  {
    package_db_ = shared_database (o.package_db_user (),
                                   o.package_db_role (),
                                   o.package_db_password (),
                                   o.package_db_name (),
                                   o.package_db_host (),
                                   o.package_db_port (),
                                   o.package_db_max_connections ());

    retry_ = retry_ < retry ? retry : retry_;
  }

  void database_module::
  init (const options::build_db& o, size_t retry)
  {
    build_db_ = shared_database (o.build_db_user (),
                                 o.build_db_role (),
                                 o.build_db_password (),
                                 o.build_db_name (),
                                 o.build_db_host (),
                                 o.build_db_port (),
                                 o.build_db_max_connections ());

    retry_ = retry_ < retry ? retry : retry_;
  }

  bool database_module::
  handle (request& rq, response& rs, log& l)
  try
  {
    return handler::handle (rq, rs, l);
  }
  catch (const odb::recoverable& e)
  {
    if (retry_-- > 0)
    {
      HANDLER_DIAG;
      l1 ([&]{trace << e << "; " << retry_ + 1 << " retries left";});
      throw retry ();
    }

    throw;
  }

  optional<string> database_module::
  update_tenant_service_state (
    const connection_ptr& conn,
    const string& type,
    const string& id,
    const function<optional<string> (const string& tenant_id,
                                     const tenant_service&)>& f)
  {
    assert (f != nullptr); // Shouldn't be called otherwise.

    // Must be initialized via the init(options::build_db) function call.
    //
    assert (build_db_ != nullptr);

    optional<string> r;

    update_tenant_service_state (
      conn, type, id,
      [&r, &f, this] (const shared_ptr<build_tenant>& t)
      {
        // Reset, in case this is a retry after the recoverable database
        // failure.
        //
        r = nullopt;

        if (t != nullptr)
        {
          assert (t->service); // Shouldn't be here otherwise.

          tenant_service& s (*t->service);

          if (optional<string> data = f (t->id, s))
          {
            s.data = move (*data);
            build_db_->update (t);

            r = move (s.data);
          }
        }
      });

    return r;
  }

  void database_module::
  update_tenant_service_state (
    const connection_ptr& conn,
    const string& type,
    const string& id,
    const function<void (const shared_ptr<build_tenant>&)>& f)
  {
    assert (f != nullptr); // Shouldn't be called otherwise.

    // Must be initialized via the init(options::build_db) function call.
    //
    assert (build_db_ != nullptr);

    for (size_t retry (retry_);;)
    {
      try
      {
        transaction tr (conn->begin ());

        using query = query<build_tenant>;

        shared_ptr<build_tenant> t (
          build_db_->query_one<build_tenant> (query::service.id == id &&
                                              query::service.type == type));

        f (t);

        tr.commit ();

        // Bail out if we have successfully updated the service state.
        //
        break;
      }
      catch (const odb::recoverable& e)
      {
        HANDLER_DIAG;

        // If no more retries left, don't re-throw odb::recoverable not to
        // retry at the upper level.
        //
        if (retry-- == 0)
          fail << e << "; no tenant service state update retries left";

        l1 ([&]{trace << e << "; " << retry + 1 << " tenant service "
                      << "state update retries left";});
      }
    }
  }
}
