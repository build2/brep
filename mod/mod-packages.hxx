// file      : mod/mod-packages.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_PACKAGES_HXX
#define MOD_MOD_PACKAGES_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/options.hxx>
#include <mod/database-module.hxx>

namespace brep
{
  class packages: public database_module
  {
  public:
    packages () = default;

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    explicit
    packages (const packages&);

    virtual bool
    handle (request&, response&);

    virtual const cli::options&
    cli_options () const {return options::packages::description ();}

  private:
    virtual void
    init (cli::scanner&);

  private:
    shared_ptr<options::packages> options_;
  };
}

#endif // MOD_MOD_PACKAGES_HXX
