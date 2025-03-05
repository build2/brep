// file      : mod/database-module.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/database-module.hxx>

#include <odb/database.hxx>
#include <odb/exceptions.hxx>
#include <odb/transaction.hxx>

#include <libbrep/build-package.hxx>
#include <libbrep/build-package-odb.hxx>

#include <mod/utility.hxx>        // sleep_before_retry()
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
        retry_max_ (r.retry_max_),
        package_db_ (r.initialized_ ? r.package_db_ : nullptr),
        build_db_ (r.initialized_ ? r.build_db_ : nullptr)
  {
  }

  void database_module::
  init (const options::package_db& o, size_t retry_max)
  {
    package_db_ = shared_database (o.package_db_user (),
                                   o.package_db_role (),
                                   o.package_db_password (),
                                   o.package_db_name (),
                                   o.package_db_host (),
                                   o.package_db_port (),
                                   o.package_db_max_connections ());

    retry_max_ = retry_max_ < retry_max ? retry_max : retry_max_;
    retry_ = 0;
  }

  void database_module::
  init (const options::build_db& o, size_t retry_max)
  {
    build_db_ = shared_database (o.build_db_user (),
                                 o.build_db_role (),
                                 o.build_db_password (),
                                 o.build_db_name (),
                                 o.build_db_host (),
                                 o.build_db_port (),
                                 o.build_db_max_connections ());

    retry_max_ = retry_max_ < retry_max ? retry_max : retry_max_;
    retry_ = 0;
  }

  bool database_module::
  handle (request& rq, response& rs, log& l)
  try
  {
    return handler::handle (rq, rs, l);
  }
  catch (const odb::recoverable& e)
  {
    if (retry_ != retry_max_)
    {
      HANDLER_DIAG;
      l1 ([&]{trace << e << "; " << retry_max_ - retry_ << " retries left";});

      sleep_before_retry (retry_++);
      throw retry ();
    }

    throw;
  }

  optional<string> database_module::
  update_tenant_service_state (
    connection_ptr& conn,
    const tenant_service_map& tsm,
    const string& type,
    const string& id,
    const function<optional<string> (const string& tenant_id,
                                     const tenant_service&)>& f)
  {
    assert (f != nullptr); // Shouldn't be called otherwise.

    // Must be initialized via the init(options::build_db) function call.
    //
    assert (build_db_ != nullptr);

    return update_tenant_service_state (
      conn, tsm, type, id,
      [&f] (const shared_ptr<build_tenant>& t)
      {
        if (t != nullptr)
        {
          assert (t->service); // Shouldn't be here otherwise.

          tenant_service& s (*t->service);

          if (optional<string> data = f (t->id, s))
          {
            s.data = move (*data);
            return true;
          }
        }

        return false;
      });
  }

  optional<string> database_module::
  update_tenant_service_state (
    connection_ptr& conn,
    const tenant_service_map& tsm,
    const string& type,
    const string& id,
    const function<bool (const shared_ptr<build_tenant>&)>& f)
  {
    assert (f != nullptr); // Shouldn't be called otherwise.

    // Must be initialized via the init(options::build_db) function call.
    //
    assert (build_db_ != nullptr);

    assert (!transaction::has_current ());

    shared_ptr<build_tenant> unsaved_state;

    for (size_t retry (0);;)
    {
      try
      {
        transaction tr (conn->begin ());

        using query = query<build_tenant>;

        shared_ptr<build_tenant> t (
          build_db_->query_one<build_tenant> (query::service.id == id &&
                                              query::service.type == type));

        // If this is our last chance to persist the service state, then stash
        // the tenant for cancellation on a potential failure to persist.
        //
        if (retry == retry_max_ && t != nullptr)
          unsaved_state = t;

        bool changed (f (t));

        if (changed)
        {
          assert (t != nullptr); // Shouldn't be here otherwise.

          build_db_->update (t);
        }

        tr.commit ();

        // Successfully updated the service state.
        //
        optional<string> r;

        if (changed)
        {
          // The f() call is only supposed to change the service state, not
          // to reset the service.
          //
          assert (t->service);

          // Note: can safely move the service data out since we cannot cancel
          // the tenant at this point.
          //
          r = move (t->service->data);
        }

        return r;
      }
      catch (const odb::recoverable& e)
      {
        HANDLER_DIAG;

        // Cancel the tenant if no more retries left. And don't re-throw
        // odb::recoverable not to retry at the upper level.
        //
        if (retry == retry_max_)
        {
          assert (unsaved_state != nullptr); // Shouldn't be here otherwise.

          // The f() call is only supposed to change the service state, not to
          // reset the service.
          //
          assert (unsaved_state->service);

          const string& tid (unsaved_state->id);
          const tenant_service& ts (*unsaved_state->service);

          error << e << "; no tenant service state update retries left, "
                << "canceling tenant " << tid << " for service " << ts.id
                << ' ' << ts.type;

          try
          {
            cancel_tenant (move (conn), retry_max_,
                           tsm,
                           log_writer_,
                           tid, ts);
          }
          catch (const runtime_error& e)
          {
            error << e << "; no retries left to cancel tenant " << tid
                  << " for service " << ts.id << ' ' << ts.type;

            // Fall through to throw.
          }

          throw server_error ();
        }

        l1 ([&]{trace << e << "; " << retry_max_ - retry << " tenant service "
                      << "state update retries left";});

        // Release the database connection before the sleep and re-acquire it
        // afterwards.
        //
        conn.reset ();
        sleep_before_retry (retry++);
        conn = build_db_->connection ();
      }
    }
  }

  void database_module::
  cancel_tenant (connection_ptr&& c,
                 size_t retry_max,
                 const tenant_service_map& tsm,
                 const diag_epilogue& log_writer,
                 const string& tid,
                 const tenant_service& ts)
  {
    using namespace odb::core;

    assert (!transaction::has_current ());

    connection_ptr conn (move (c)); // Make sure the connection is released.

    database& db (conn->database ());

    for (size_t retry (0);;)
    {
      try
      {
        transaction tr (conn->begin ());

        shared_ptr<build_tenant> t (db.find<build_tenant> (tid));

        // Note: if already archived, don't call the callback below. Failed
        // that, we will likely call it multiple times.
        //
        if (t == nullptr || t->archived)
          return;

        t->archived = true;
        db.update (t);
        tr.commit ();

        // Bail out if we have successfully archived the tenant.
        //
        break;
      }
      catch (const odb::recoverable& e)
      {
        // If no more retries left, don't re-throw odb::recoverable not to
        // retry at the upper level.
        //
        if (retry == retry_max)
          throw runtime_error (e.what ());

        // Try to cancel as fast as possible, so don't sleep.
        //
        retry++;
      }
    }

    // Release the database connection since the build_canceled() notification
    // can potentially be time-consuming (e.g., it may perform an HTTP
    // request).
    //
    conn.reset ();

    // Now, as the tenant is successfully canceled, call the build canceled
    // notification.
    //
    {
      auto i (tsm.find (ts.type));

      if (i != tsm.end ())
      {
        if (auto tsb =
            dynamic_cast<const tenant_service_build_built*> (i->second.get ()))
        {
          tsb->build_canceled (tid, ts, log_writer);
        }
      }
    }
  }
}
