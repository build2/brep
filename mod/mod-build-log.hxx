// file      : mod/mod-build-log.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_BUILD_LOG_HXX
#define MOD_MOD_BUILD_LOG_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/options.hxx>
#include <mod/database-module.hxx>
#include <mod/build-config-module.hxx>

namespace brep
{
  class build_log: public database_module, private build_config_module
  {
  public:
    build_log () = default;

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    explicit
    build_log (const build_log&);

    virtual bool
    handle (request&, response&);

    virtual const cli::options&
    cli_options () const
    {
      return options::build_log::description ();
    }

  private:
    virtual void
    init (cli::scanner&);

  private:
    shared_ptr<options::build_log> options_;
  };
}

#endif // MOD_MOD_BUILD_LOG_HXX
