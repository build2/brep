// file      : mod/mod-advanced-search.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_ADVANCED_SEARCH_HXX
#define MOD_MOD_ADVANCED_SEARCH_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/module-options.hxx>
#include <mod/database-module.hxx>

namespace brep
{
  class advanced_search: public database_module
  {
  public:
    advanced_search () = default;

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    explicit
    advanced_search (const advanced_search&);

    virtual bool
    handle (request&, response&);

    virtual const cli::options&
    cli_options () const {return options::advanced_search::description ();}

  private:
    virtual void
    init (cli::scanner&);

  private:
    shared_ptr<options::advanced_search> options_;
  };
}

#endif // MOD_MOD_ADVANCED_SEARCH_HXX
