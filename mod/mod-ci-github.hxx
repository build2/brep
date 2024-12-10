// file      : mod/mod-ci-github.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_CI_GITHUB_HXX
#define MOD_MOD_CI_GITHUB_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/module.hxx>
#include <mod/module-options.hxx>

#include <mod/ci-common.hxx>
#include <mod/tenant-service.hxx>

#include <mod/mod-ci-github-gh.hxx>

namespace brep
{
  class ci_github: public handler,
                   private ci_start,
                   public tenant_service_build_queued,
                   public tenant_service_build_building,
                   public tenant_service_build_built
  {
  public:
    explicit
    ci_github (tenant_service_map&);

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    explicit
    ci_github (const ci_github&, tenant_service_map&);

    virtual bool
    handle (request&, response&);

    virtual const cli::options&
    cli_options () const {return options::ci_github::description ();}

    virtual function<optional<string> (const tenant_service&)>
    build_queued (const tenant_service&,
                  const vector<build>&,
                  optional<build_state> initial_state,
                  const build_queued_hints&,
                  const diag_epilogue& log_writer) const noexcept override;

    virtual function<optional<string> (const tenant_service&)>
    build_building (const tenant_service&, const build&,
                    const diag_epilogue& log_writer) const noexcept override;

    virtual function<optional<string> (const tenant_service&)>
    build_built (const tenant_service&, const build&,
                 const diag_epilogue& log_writer) const noexcept override;

  private:
    virtual void
    init (cli::scanner&);

    // Handle the check_suite event `requested` and `rerequested` actions.
    //
    bool
    handle_check_suite_request (gh_check_suite_event);

    optional<string>
    generate_jwt (const basic_mark& trace, const basic_mark& error) const;

    // Authenticate to GitHub as an app installation.
    //
    optional<gh_installation_access_token>
    obtain_installation_access_token (uint64_t install_id,
                                      string jwt,
                                      const basic_mark& error) const;

  private:
    shared_ptr<options::ci_github> options_;

    tenant_service_map& tenant_service_map_;
  };
}

#endif // MOD_MOD_CI_GITHUB_HXX
