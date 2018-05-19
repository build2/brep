// file      : mod/mod-package-search.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_PACKAGE_SEARCH_HXX
#define MOD_MOD_PACKAGE_SEARCH_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/options.hxx>
#include <mod/database-module.hxx>

namespace brep
{
  class package_search: public database_module
  {
  public:
    package_search () = default;

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    explicit
    package_search (const package_search&);

    virtual bool
    handle (request&, response&);

    virtual const cli::options&
    cli_options () const {return options::package_search::description ();}

  private:
    virtual void
    init (cli::scanner&);

  private:
    shared_ptr<options::package_search> options_;
  };
}

#endif // MOD_MOD_PACKAGE_SEARCH_HXX
