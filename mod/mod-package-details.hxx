// file      : mod/mod-package-details.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_PACKAGE_DETAILS_HXX
#define MOD_MOD_PACKAGE_DETAILS_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/options.hxx>
#include <mod/database-module.hxx>

namespace brep
{
  class package_details: public database_module
  {
  public:
    package_details () = default;

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    explicit
    package_details (const package_details&);

    virtual bool
    handle (request&, response&);

    virtual const cli::options&
    cli_options () const {return options::package_details::description ();}

  private:
    virtual void
    init (cli::scanner&);

  private:
    shared_ptr<options::package_details> options_;
  };
}

#endif // MOD_MOD_PACKAGE_DETAILS_HXX
