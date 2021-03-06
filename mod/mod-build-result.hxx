// file      : mod/mod-build-result.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_BUILD_RESULT_HXX
#define MOD_MOD_BUILD_RESULT_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/module-options.hxx>
#include <mod/database-module.hxx>
#include <mod/build-config-module.hxx>

namespace brep
{
  class build_result: public database_module, private build_config_module
  {
  public:
    build_result () = default;

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    explicit
    build_result (const build_result&);

    virtual bool
    handle (request&, response&);

    virtual const cli::options&
    cli_options () const {return options::build_result::description ();}

  private:
    virtual void
    init (cli::scanner&);

  private:
    shared_ptr<options::build_result> options_;
  };
}

#endif // MOD_MOD_BUILD_RESULT_HXX
