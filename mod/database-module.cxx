// file      : mod/database-module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/database-module>

#include <odb/exceptions.hxx>

#include <mod/options>
#include <mod/database>

namespace brep
{
  // While currently the user-defined copy constructor is not required (we
  // don't need to deep copy nullptr's), it is a good idea to keep the
  // placeholder ready for less trivial cases.
  //
  database_module::
  database_module (const database_module& r)
      : module (r),
        retry_ (r.retry_),
        db_ (r.initialized_ ? r.db_ : nullptr)
  {
  }

  void database_module::
  init (const options::db& o)
  {
    retry_ = o.db_retry ();
    db_ = shared_database (o);
  }

  bool database_module::
  handle (request& rq, response& rs, log& l)
  try
  {
    return module::handle (rq, rs, l);
  }
  catch (const odb::recoverable& e)
  {
    if (retry_-- > 0)
    {
      MODULE_DIAG;
      l1 ([&]{trace << e.what () << "; " << retry_ + 1 << " retries left";});
      throw retry ();
    }

    throw;
  }
}
