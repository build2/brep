// file      : mod/database-module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/database-module.hxx>

#include <errno.h> // EIO

#include <sstream>

#include <odb/exceptions.hxx>

#include <libbutl/utility.hxx> // throw_generic_error()

#include <mod/options.hxx>
#include <mod/database.hxx>
#include <mod/build-config.hxx>

namespace brep
{
  using namespace std;
  using namespace butl;
  using namespace bbot;

  // While currently the user-defined copy constructor is not required (we
  // don't need to deep copy nullptr's), it is a good idea to keep the
  // placeholder ready for less trivial cases.
  //
  database_module::
  database_module (const database_module& r)
      : module (r),
        retry_ (r.retry_),
        package_db_ (r.initialized_ ? r.package_db_ : nullptr),
        build_db_ (r.initialized_ ? r.build_db_ : nullptr),
        build_conf_ (r.initialized_ ? r.build_conf_ : nullptr),
        build_conf_names_ (r.initialized_ ? r.build_conf_names_ : nullptr),
        build_conf_map_ (r.initialized_ ? r.build_conf_map_ : nullptr)
  {
  }

  void database_module::
  init (const options::package_db& o, size_t retry)
  {
    package_db_ = shared_database (o.package_db_user (),
                                   o.package_db_password (),
                                   o.package_db_name (),
                                   o.package_db_host (),
                                   o.package_db_port (),
                                   o.package_db_max_connections ());

    retry_ = retry_ < retry ? retry : retry_;
  }

  void database_module::
  init (const options::build& bo, const options::build_db& dbo, size_t retry)
  {
    try
    {
      build_conf_ = shared_build_config (bo.build_config ());
    }
    catch (const io_error& e)
    {
      ostringstream os;
      os << "unable to read build configuration '" << bo.build_config ()
         << "': " << e;

      throw_generic_error (EIO, os.str ().c_str ());
    }

    cstrings conf_names;

    using conf_map_type = map<const char*,
                              const build_config*,
                              compare_c_string>;
    conf_map_type conf_map;

    for (const auto& c: *build_conf_)
    {
      const char* cn (c.name.c_str ());
      conf_map[cn] = &c;
      conf_names.push_back (cn);
    }

    build_conf_names_ = make_shared<cstrings> (move (conf_names));
    build_conf_map_ = make_shared<conf_map_type> (move (conf_map));

    build_db_ = shared_database (dbo.build_db_user (),
                                 dbo.build_db_password (),
                                 dbo.build_db_name (),
                                 dbo.build_db_host (),
                                 dbo.build_db_port (),
                                 dbo.build_db_max_connections ());

    retry_ = retry_ < retry ? retry : retry_;
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
      l1 ([&]{trace << e << "; " << retry_ + 1 << " retries left";});
      throw retry ();
    }

    throw;
  }
}
