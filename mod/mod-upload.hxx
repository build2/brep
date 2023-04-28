// file      : mod/mod-upload.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_UPLOAD_HXX
#define MOD_MOD_UPLOAD_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/module-options.hxx>
#include <mod/build-result-module.hxx>

namespace brep
{
  class upload: public build_result_module
  {
  public:
    upload () = default;

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    explicit
    upload (const upload&);

    virtual bool
    handle (request&, response&);

    virtual const cli::options&
    cli_options () const {return options::upload::description ();}

  private:
    virtual void
    init (cli::scanner&);

  private:
    shared_ptr<options::upload> options_;
  };
}

#endif // MOD_MOD_UPLOAD_HXX
