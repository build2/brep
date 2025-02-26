// file      : mod/mod-ci.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_CI_HXX
#define MOD_MOD_CI_HXX

#include <web/xhtml/fragment.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <libbrep/build.hxx>
#include <libbrep/common.hxx> // tenant_service

#include <mod/module.hxx>
#include <mod/module-options.hxx>

#include <mod/ci-common.hxx>
#include <mod/database-module.hxx>

#if defined(BREP_CI_TENANT_SERVICE_UNLOADED) && !defined(BREP_CI_TENANT_SERVICE)
#  error BREP_CI_TENANT_SERVICE must be defined if BREP_CI_TENANT_SERVICE_UNLOADED is defined
#endif

#ifdef BREP_CI_TENANT_SERVICE
#  include <mod/tenant-service.hxx>
#endif

namespace brep
{
  class ci:
#ifndef BREP_CI_TENANT_SERVICE_UNLOADED
            public handler,
#else
            public database_module,
#endif
            private ci_start
#ifdef BREP_CI_TENANT_SERVICE
          , public tenant_service_build_queued,
            public tenant_service_build_building,
            public tenant_service_build_built
#ifdef BREP_CI_TENANT_SERVICE_UNLOADED
          , public tenant_service_build_unloaded
#endif
#endif
  {
  public:

#ifdef BREP_CI_TENANT_SERVICE
    explicit
    ci (tenant_service_map&);

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    ci (const ci&, tenant_service_map&);
#else
    ci () = default;

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    explicit
    ci (const ci&);
#endif

    virtual bool
    handle (request&, response&) override;

    virtual const cli::options&
    cli_options () const override {return options::ci::description ();}

#ifdef BREP_CI_TENANT_SERVICE
    virtual function<optional<string> (const string& tenant_id,
                                       const tenant_service&)>
    build_queued (const string& tenant_id,
                  const tenant_service&,
                  const vector<build>&,
                  optional<build_state> initial_state,
                  const build_queued_hints&,
                  const diag_epilogue& log_writer) const noexcept override;

    virtual function<optional<string> (const string& tenant_id,
                                       const tenant_service&)>
    build_building (const string& tenant_id,
                    const tenant_service&,
                    const build&,
                    const diag_epilogue& log_writer) const noexcept override;

    virtual function<pair<optional<string>, bool> (const string& tenant_id,
                                                   const tenant_service&)>
    build_built (const string& tenant_id,
                 const tenant_service&,
                 const build&,
                 const diag_epilogue& log_writer) const noexcept override;

#ifdef BREP_CI_TENANT_SERVICE_UNLOADED
    virtual function<optional<string> (const string& tenant_id,
                                       const tenant_service&)>
    build_unloaded (const string& tenant_id,
                    tenant_service&&,
                    const diag_epilogue& log_writer) const noexcept override;
#endif
#endif

  private:
    virtual void
    init (cli::scanner&) override;

  private:
    shared_ptr<options::ci> options_;
    shared_ptr<web::xhtml::fragment> form_;

#ifdef BREP_CI_TENANT_SERVICE
    tenant_service_map& tenant_service_map_;
#endif
  };

  class ci_cancel: public database_module,
                   private ci_start
  {
  public:
    ci_cancel () = default;

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    explicit
    ci_cancel (const ci_cancel&);

    virtual bool
    handle (request&, response&) override;

    virtual const cli::options&
    cli_options () const override {return options::ci_cancel::description ();}

  private:
    virtual void
    init (cli::scanner&) override;

  private:
    shared_ptr<options::ci_cancel> options_;
  };
}

#endif // MOD_MOD_CI_HXX
