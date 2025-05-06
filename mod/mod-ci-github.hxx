// file      : mod/mod-ci-github.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_CI_GITHUB_HXX
#define MOD_MOD_CI_GITHUB_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/module-options.hxx>
#include <mod/database-module.hxx>

#include <mod/ci-common.hxx>
#include <mod/tenant-service.hxx>

#include <mod/mod-ci-github-gh.hxx>

namespace brep
{
  struct service_data;

  class ci_github: public database_module,
                   private ci_start,
                   public tenant_service_build_unloaded,
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
    handle (request&, response&) override;

    virtual const cli::options&
    cli_options () const override {return options::ci_github::description ();}

    virtual function<optional<string> (const string&, const tenant_service&)>
    build_unloaded (const string& tenant_id,
                    tenant_service&&,
                    const diag_epilogue& log_writer) const noexcept override;

    function<optional<string> (const string&, const tenant_service&)>
    build_unloaded_pre_check (tenant_service&&,
                              service_data&&,
                              const diag_epilogue&) const noexcept;

    function<optional<string> (const string&, const tenant_service&)>
    build_unloaded_load (const string& tenant_id,
                         tenant_service&&,
                         service_data&&,
                         const diag_epilogue&) const noexcept;

    virtual function<optional<string> (const string&, const tenant_service&)>
    build_queued (const string& tenant_id,
                  const tenant_service&,
                  const vector<build>&,
                  optional<build_state> initial_state,
                  const build_queued_hints&,
                  const diag_epilogue& log_writer) const noexcept override;

    virtual function<optional<string> (const string&, const tenant_service&)>
    build_building (const string& tenant_id,
                    const tenant_service&,
                    const build&,
                    const diag_epilogue& log_writer) const noexcept override;

    virtual function<pair<optional<string>, bool> (const string&,
                                                   const tenant_service&)>
    build_built (const string& tenant_id,
                 const tenant_service&,
                 const build&,
                 const diag_epilogue& log_writer) const noexcept override;

    virtual void
    build_completed (const string& tenant_id,
                     const tenant_service& ts,
                     const diag_epilogue& log_writer) const noexcept override;

    virtual void
    build_canceled (const string& tenant_id,
                    const tenant_service&,
                    const diag_epilogue& log_writer) const noexcept override;

  private:
    virtual void
    init (cli::scanner&) override;

    // Handle push events (branch push).
    //
    // If warning_success is true, then map result_status::warning to SUCCESS
    // and to FAILURE otherwise.
    //
    bool
    handle_branch_push (gh_push_event, bool warning_success);

    // Handle the pull_request event `opened` and `synchronize` actions.
    //
    // If warning_success is true, then map result_status::warning to SUCCESS
    // and to FAILURE otherwise.
    //
    bool
    handle_pull_request (gh_pull_request_event, bool warning_success);

    // Handle the check_suite event `rerequested` action.
    //
    // If warning_success is true, then map result_status::warning to SUCCESS
    // and to FAILURE otherwise.
    //
    bool
    handle_check_suite_rerequest (gh_check_suite_event, bool warning_success);

    // Handle the check_suite event `completed` action.
    //
    // If warning_success is true, then map result_status::warning to SUCCESS
    // and to FAILURE otherwise.
    //
    bool
    handle_check_suite_completed (gh_check_suite_event, bool warning_success);

    // Handle the check_run event `rerequested` action.
    //
    // If warning_success is true, then map result_status::warning to SUCCESS
    // and to FAILURE otherwise.
    //
    bool
    handle_check_run_rerequest (const gh_check_run_event&, bool warning_success);

    // Handle forced check suite rebuild.
    //
    // A forced rebuild is a last-resort, user-initiated process used to
    // recover from an inconsistent check run state on GitHub. For example, if
    // all build check runs have been updated on GitHub as successfully
    // completed but the final conclusion check run update fails -- leaving it
    // in progress -- none of the various check run re-requesting options will
    // be presented in the GitHub UI. In such cases the force rebuild link in
    // the conclusion check run description can be used to trigger a rebuild
    // of the entire check suite (all check runs).
    //
    bool
    handle_forced_check_suite_rebuild (const name_values& query_parameters,
                                       response&);

    // Construct and return the conclusion check run name for the specified
    // GitHub App id.
    //
    // Append the GitHub App name to the conclusion check run basename in
    // order to provide additional context where it is lacking in certain
    // parts of the GitHub UI.
    //
    // Throw invalid_argument if no app name is configured for this app id.
    //
    string
    conclusion_check_run_name (uint64_t app_id) const;

    // Build a check run details_url for a build.
    //
    string
    details_url (const build&) const;

    // Build a check run details_url for a tenant.
    //
    string
    details_url (const string& tenant) const;

    // Generate a force rebuild link in Markdown. For example:
    //
    //   [Force rebuild](https://...)
    //
    string
    force_rebuild_md_link (const service_data&) const;

    optional<string>
    generate_jwt (uint64_t app_id,
                  const basic_mark& trace,
                  const basic_mark& error) const;

    // Authenticate to GitHub as an app installation. Return the installation
    // access token (IAT). Issue diagnostics and return nullopt if something
    // goes wrong.
    //
    optional<gh_installation_access_token>
    obtain_installation_access_token (const string& install_id,
                                      string jwt,
                                      const basic_mark& error) const;

  private:
    shared_ptr<options::ci_github> options_;

    tenant_service_map& tenant_service_map_;

    string webhook_secret_;
  };
}

#endif // MOD_MOD_CI_GITHUB_HXX
