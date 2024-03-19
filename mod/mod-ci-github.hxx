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

namespace butl
{
  namespace json
  {
    class parser;
  }
}

namespace brep
{
  // GitHub request/response types.
  //
  // Note that the GitHub REST and GraphQL APIs use different ID types and
  // values. In the REST API they are usually integers (but sometimes
  // strings!) whereas in GraphQL they are always strings (note:
  // base64-encoded and opaque, not just the REST ID value as a string).
  //
  // In both APIs the ID field is called `id`, but REST responses and webhook
  // events also contain the corresponding GraphQL object's ID in the
  // `node_id` field.
  //
  // In the structures below we always use the RESP API/webhook names for ID
  // fields. I.e., `id` always refers to the REST/webhook ID, and `node_id`
  // always refers to the GraphQL ID.
  //
  // Note that having the below types directly in brep causes clashes (e.g.,
  // for the repository name).
  //
  namespace gh
  {
    namespace json = butl::json;

    // The "check_suite" object within a check_suite webhook event request.
    //
    struct check_suite
    {
      uint64_t id; // Note: used for installation access token (REST API).
      string node_id;
      string head_branch;
      string head_sha;
      string before;
      string after;

      explicit
      check_suite (json::parser&);

      check_suite () = default;
    };

    struct check_run
    {
      string node_id;
      string name;
      string status;

      explicit
      check_run (json::parser&);

      check_run () = default;
    };

    struct repository
    {
      string node_id;
      string name;
      string full_name;
      string default_branch;
      string clone_url;

      explicit
      repository (json::parser&);

      repository () = default;
    };

    struct installation
    {
      uint64_t id;

      explicit
      installation (json::parser&);

      installation () = default;
    };

    // The check_suite webhook event request.
    //
    struct check_suite_event
    {
      string action;
      gh::check_suite check_suite;
      gh::repository repository;
      gh::installation installation;

      explicit
      check_suite_event (json::parser&);

      check_suite_event () = default;
    };

    struct installation_access_token
    {
      string token;
      timestamp expires_at;

      explicit
      installation_access_token (json::parser&);

      installation_access_token () = default;
    };

    ostream&
    operator<< (ostream&, const check_suite&);

    ostream&
    operator<< (ostream&, const check_run&);

    ostream&
    operator<< (ostream&, const repository&);

    ostream&
    operator<< (ostream&, const installation&);

    ostream&
    operator<< (ostream&, const check_suite_event&);

    ostream&
    operator<< (ostream&, const installation_access_token&);
  }

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
                  const fail_mark<server_error>& fail,
                  const basic_mark& error,
                  const basic_mark& warn,
                  const basic_mark& trace) const override;

    virtual function<optional<string> (const tenant_service&)>
    build_building (const tenant_service&, const build&,
                    const fail_mark<server_error>& fail,
                    const basic_mark& error,
                    const basic_mark& warn,
                    const basic_mark& trace) const override;

    virtual function<optional<string> (const tenant_service&)>
    build_built (const tenant_service&, const build&,
                 const fail_mark<server_error>& fail,
                 const basic_mark& error,
                 const basic_mark& warn,
                 const basic_mark& trace) const override;

  private:
    virtual void
    init (cli::scanner&);

    // Handle the check_suite event `requested` and `rerequested` actions.
    //
    bool
    handle_check_suite_request (gh::check_suite_event);

    string
    generate_jwt () const;

    // Authenticate to GitHub as an app installation.
    //
    gh::installation_access_token
    obtain_installation_access_token (uint64_t install_id, string jwt) const;

  private:
    shared_ptr<options::ci_github> options_;

    tenant_service_map& tenant_service_map_;
  };
}

#endif // MOD_MOD_CI_GITHUB_HXX
