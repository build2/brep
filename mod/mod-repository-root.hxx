// file      : mod/mod-repository-root.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_REPOSITORY_ROOT_HXX
#define MOD_MOD_REPOSITORY_ROOT_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/module.hxx>
#include <mod/options.hxx>

namespace brep
{
  class packages;
  class package_details;
  class package_version_details;
  class repository_details;
  class build_task;
  class build_result;
  class build_force;
  class build_log;
  class builds;
  class build_configs;
  class submit;
  class ci;

  class repository_root: public handler
  {
  public:
    repository_root ();

    // Copy constructible-only type.
    //
    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    explicit
    repository_root (const repository_root&);

  private:
    virtual bool
    handle (request&, response&);

    virtual const cli::options&
    cli_options () const {return options::repository_root::description ();}

    virtual option_descriptions
    options ();

    virtual void
    init (const name_values&);

    virtual void
    init (cli::scanner&);

    virtual void
    version ();

  private:
    shared_ptr<packages> packages_;
    shared_ptr<package_details> package_details_;
    shared_ptr<package_version_details> package_version_details_;
    shared_ptr<repository_details> repository_details_;
    shared_ptr<build_task> build_task_;
    shared_ptr<build_result> build_result_;
    shared_ptr<build_force> build_force_;
    shared_ptr<build_log> build_log_;
    shared_ptr<builds> builds_;
    shared_ptr<build_configs> build_configs_;
    shared_ptr<submit> submit_;
    shared_ptr<ci> ci_;

    shared_ptr<options::repository_root> options_;

    // Sub-handler the request is dispatched to. Initially is NULL. It is set
    // by the first call to handle() to a deep copy of the selected exemplar.
    // The subsequent calls of handle() (that may take place after the retry
    // exception is thrown) will use the existing handler instance.
    //
    unique_ptr<handler> handler_;
  };
}

#endif // MOD_MOD_REPOSITORY_ROOT_HXX
