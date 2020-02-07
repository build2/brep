// file      : mod/mod-submit.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_SUBMIT_HXX
#define MOD_MOD_SUBMIT_HXX

#include <web/xhtml-fragment.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/module.hxx>
#include <mod/options.hxx>

namespace brep
{
  class submit: public handler
  {
  public:
    submit () = default;

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    explicit
    submit (const submit&);

    virtual bool
    handle (request&, response&);

    virtual const cli::options&
    cli_options () const {return options::submit::description ();}

  private:
    virtual void
    init (cli::scanner&);

  private:
    shared_ptr<options::submit> options_;
    shared_ptr<web::xhtml::fragment> form_;
  };
}

#endif // MOD_MOD_SUBMIT_HXX
