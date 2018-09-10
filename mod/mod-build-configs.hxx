// file      : mod/mod-build-configs.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_BUILD_CONFIGS_HXX
#define MOD_MOD_BUILD_CONFIGS_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <libbbot/build-config.hxx>

#include <mod/module.hxx>
#include <mod/options.hxx>

namespace brep
{
  class build_configs: public handler
  {
  public:
    build_configs () = default;

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    explicit
    build_configs (const build_configs&);

    virtual bool
    handle (request&, response&);

    virtual const cli::options&
    cli_options () const
    {
      return options::build_configs::description ();
    }

  private:
    virtual void
    init (cli::scanner&);

  private:
    shared_ptr<options::build_configs> options_;
    shared_ptr<const bbot::build_configs> build_conf_;
  };
}

#endif // MOD_MOD_BUILD_CONFIGS_HXX
