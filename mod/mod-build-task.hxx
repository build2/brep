// file      : mod/mod-build-task.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_BUILD_TASK_HXX
#define MOD_MOD_BUILD_TASK_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/options.hxx>
#include <mod/database-module.hxx>
#include <mod/build-config-module.hxx>

namespace brep
{
  class build_task: public database_module, private build_config_module
  {
  public:
    build_task () = default;

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    explicit
    build_task (const build_task&);

    virtual bool
    handle (request&, response&);

    virtual const cli::options&
    cli_options () const {return options::build_task::description ();}

  private:
    virtual void
    init (cli::scanner&);

  private:
    shared_ptr<options::build_task> options_;
  };
}

#endif // MOD_MOD_BUILD_TASK_HXX
