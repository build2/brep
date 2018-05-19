// file      : mod/mod-package-version-details.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_PACKAGE_VERSION_DETAILS_HXX
#define MOD_MOD_PACKAGE_VERSION_DETAILS_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/options.hxx>
#include <mod/database-module.hxx>

namespace brep
{
  class package_version_details: public database_module
  {
  public:
    package_version_details () = default;

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    explicit
    package_version_details (const package_version_details&);

    virtual bool
    handle (request&, response&);

    virtual const cli::options&
    cli_options () const
    {
      return options::package_version_details::description ();
    }

  private:
    virtual void
    init (cli::scanner&);

  private:
    shared_ptr<options::package_version_details> options_;
  };
}

#endif // MOD_MOD_PACKAGE_VERSION_DETAILS_HXX
