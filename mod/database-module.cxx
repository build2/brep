// file      : mod/database-module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/database-module.hxx>

#include <odb/exceptions.hxx>

#include <mod/options.hxx>
#include <mod/database.hxx>

namespace brep
{
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
}
