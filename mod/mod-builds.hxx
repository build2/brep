// file      : mod/mod-builds.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_BUILDS_HXX
#define MOD_MOD_BUILDS_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/options.hxx>
#include <mod/database-module.hxx>

namespace brep
{
  class builds: public database_module
  {
  public:
    builds () = default;

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    explicit
    builds (const builds&);

    virtual bool
    handle (request&, response&);

    virtual const cli::options&
    cli_options () const {return options::builds::description ();}

  private:
    virtual void
    init (cli::scanner&);

  private:
    shared_ptr<options::builds> options_;
  };
}

#endif // MOD_MOD_BUILDS_HXX
