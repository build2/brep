// file      : mod/mod-ci.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_CI_HXX
#define MOD_MOD_CI_HXX

#include <web/xhtml/fragment.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/module.hxx>
#include <mod/module-options.hxx>

namespace brep
{
  class ci: public handler
  {
  public:
    ci () = default;

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    explicit
    ci (const ci&);

    virtual bool
    handle (request&, response&);

    virtual const cli::options&
    cli_options () const {return options::ci::description ();}

  private:
    virtual void
    init (cli::scanner&);

  private:
    shared_ptr<options::ci> options_;
    shared_ptr<web::xhtml::fragment> form_;
  };
}

#endif // MOD_MOD_CI_HXX
