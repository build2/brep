// file      : mod/mod-build-result.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_BUILD_RESULT_HXX
#define MOD_MOD_BUILD_RESULT_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/module-options.hxx>
#include <mod/tenant-service.hxx>
#include <mod/build-result-module.hxx>

namespace brep
{
  class build_result: public build_result_module
  {
  public:
    explicit
    build_result (const tenant_service_map&);

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    build_result (const build_result&, const tenant_service_map&);

    virtual bool
    handle (request&, response&);

    virtual const cli::options&
    cli_options () const {return options::build_result::description ();}

  private:
    virtual void
    init (cli::scanner&);

  private:
    shared_ptr<options::build_result> options_;
    const tenant_service_map& tenant_service_map_;
  };
}

#endif // MOD_MOD_BUILD_RESULT_HXX
