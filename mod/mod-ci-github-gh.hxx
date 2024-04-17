// file      : mod/mod-ci-github-gh.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_CI_GITHUB_GH_HXX
#define MOD_MOD_CI_GITHUB_GH_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

namespace butl
{
  namespace json
  {
    class parser;
  }
}

namespace brep
{
  // GitHub request/response types (all start with gh_).
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
  namespace json = butl::json;

  // The "check_suite" object within a check_suite webhook event request.
  //
  struct gh_check_suite
  {
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

  // Return the GitHub check run status corresponding to a build_state.
  //
  string
  gh_to_status (build_state st)
  {
    // @@ Just return by value (small string optimization).
    //
    static const string sts[] {"QUEUED", "IN_PROGRESS", "COMPLETED"};

    return sts[static_cast<size_t> (st)];
  }

  // Return the build_state corresponding to a GitHub check run status
  // string. Throw invalid_argument if the passed status was invalid.
  //
  build_state
  gh_from_status (const string& s)
  {
    if      (s == "QUEUED")      return build_state::queued;
    else if (s == "IN_PROGRESS") return build_state::building;
    else if (s == "COMPLETED")   return build_state::built;
    else
      throw invalid_argument ("invalid GitHub check run status: '" + s +
                              '\'');
  }

  // Create a check_run name from a build. If the second argument is not
  // NULL, return an abbreviated id if possible.
  //
  string
  gh_check_run_name (const build& b,
                     const tenant_service_base::build_hints* bh = nullptr)
  {
    string r;

    if (bh == nullptr || !bh->single_package_version)
    {
      r += b.package_name.string ();
      r += '/';
      r += b.package_version.string ();
      r += '/';
    }

    r += b.target_config_name;
    r += '/';
    r += b.target.string ();
    r += '/';

    if (bh == nullptr || !bh->single_package_config)
    {
      r += b.package_config_name;
      r += '/';
    }

    r += b.toolchain_name;
    r += '-';
    r += b.toolchain_version.string ();

    return r;
  }

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
    uint64_t id; // Note: used for installation access token (REST API).

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

    installation_access_token (string token, timestamp expires_at);

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

#endif // MOD_MOD_CI_GITHUB_GH_HXX
