// file      : mod/mod-build-task.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_BUILD_TASK_HXX
#define MOD_MOD_BUILD_TASK_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/module-options.hxx>
#include <mod/tenant-service.hxx>
#include <mod/database-module.hxx>
#include <mod/build-config-module.hxx>

namespace brep
{
  class build_task: public database_module, private build_config_module
  {
  public:
    explicit
    build_task (const tenant_service_map&);

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    build_task (const build_task&, const tenant_service_map&);

    virtual bool
    handle (request&, response&);

    virtual const cli::options&
    cli_options () const {return options::build_task::description ();}

  private:
    virtual void
    init (cli::scanner&);

  private:
    shared_ptr<options::build_task> options_;
    const tenant_service_map& tenant_service_map_;
  };
}

#endif // MOD_MOD_BUILD_TASK_HXX
