// file      : mod/mod-ci-github.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_CI_GITHUB_HXX
#define MOD_MOD_CI_GITHUB_HXX

#include <web/xhtml/fragment.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/module.hxx>
#include <mod/module-options.hxx>

namespace brep
{
  class ci_github: public handler
  {
  public:
    ci_github () = default;

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    explicit
    ci_github (const ci_github&);

    virtual bool
    handle (request&, response&);

    virtual const cli::options&
    cli_options () const {return options::ci::description ();}

  private:
    virtual void
    init (cli::scanner&);

    // @@ Can it be static in .cxx file?
    //
    bool
    respond (response&, status_code, const string& message);

  private:
    shared_ptr<options::ci> options_;
  };
}

#endif // MOD_MOD_CI_GITHUB_HXX
