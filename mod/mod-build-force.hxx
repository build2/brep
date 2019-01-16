// file      : mod/mod-build-force.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_BUILD_FORCE_HXX
#define MOD_MOD_BUILD_FORCE_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/options.hxx>
#include <mod/database-module.hxx>
#include <mod/build-config-module.hxx>

namespace brep
{
  class build_force: public database_module, private build_config_module
  {
  public:
    build_force () = default;

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    explicit
    build_force (const build_force&);

    virtual bool
    handle (request&, response&);

    virtual const cli::options&
    cli_options () const
    {
      return options::build_force::description ();
    }

  private:
    virtual void
    init (cli::scanner&);

  private:
    shared_ptr<options::build_force> options_;
  };
}

#endif // MOD_MOD_BUILD_FORCE_HXX
