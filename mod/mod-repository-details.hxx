// file      : mod/mod-repository-details.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_REPOSITORY_DETAILS_HXX
#define MOD_MOD_REPOSITORY_DETAILS_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/options.hxx>
#include <mod/database-module.hxx>

namespace brep
{
  class repository_details: public database_module
  {
  public:
    repository_details () = default;

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    explicit
    repository_details (const repository_details&);

    virtual bool
    handle (request&, response&);

    virtual const cli::options&
    cli_options () const {return options::repository_details::description ();}

  private:
    virtual void
    init (cli::scanner&);

  private:
    shared_ptr<options::repository_details> options_;
  };
}

#endif // MOD_MOD_REPOSITORY_DETAILS_HXX
