// file      : mod/mod-ci-github-gh.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_CI_GITHUB_GH_HXX
#define MOD_MOD_CI_GITHUB_GH_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <libbrep/build.hxx>

#include <mod/tenant-service.hxx> // build_hints

namespace butl
{
  namespace json
  {
    class parser;
  }
}

namespace brep
{
  using build_queued_hints = tenant_service_build_queued::build_queued_hints;

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

    explicit
    gh_check_suite (json::parser&);

    gh_check_suite () = default;
  };

  struct gh_check_run
  {
    string node_id;
    string name;
    string status;

    explicit
    gh_check_run (json::parser&);

    gh_check_run () = default;
  };

  struct gh_pull_request
  {
    string node_id;
    unsigned int number;

    string state; // "open" or "closed".

    // If absent then the result of the test merge commit is not yet
    // available. If true then `merge_commit_sha` contains the commit ID of
    // the merge commit. If false then `merge_commit_sha` is either empty or
    // no longer valid.
    //
    optional<bool> mergeable;
    string merge_commit_sha;

    string base_fullname;
    string base_ref; // @@ TODO Remove if remains unused.
    string base_sha; // @@ TODO Remove if remains unused.

    string head_fullname;
    string head_ref; // @@ TODO Remove if remains unused.
    string head_sha;

    explicit
    gh_pull_request (json::parser&);

    gh_pull_request () = default;
  };

  // Return the GitHub check run status corresponding to a build_state.
  //
  string
  gh_to_status (build_state st);

  // Return the build_state corresponding to a GitHub check run status
  // string. Throw invalid_argument if the passed status was invalid.
  //
  build_state
  gh_from_status (const string&);

  // If warning_success is true, then map result_status::warning to SUCCESS
  // and to FAILURE otherwise.
  //
  string
  gh_to_conclusion (result_status, bool warning_success);

  // Create a check_run name from a build. If the second argument is not
  // NULL, return an abbreviated id if possible.
  //
  string
  gh_check_run_name (const build&, const build_queued_hints* = nullptr);

  struct gh_repository
  {
    string node_id;
    string name;
    string full_name;
    string default_branch;
    string clone_url;

    explicit
    gh_repository (json::parser&);

    gh_repository () = default;
  };

  struct gh_installation
  {
    uint64_t id; // Note: used for installation access token (REST API).

    explicit
    gh_installation (json::parser&);

    gh_installation () = default;
  };

  // The check_suite webhook event request.
  //
  struct gh_check_suite_event
  {
    string action;
    gh_check_suite check_suite;
    gh_repository repository;
    gh_installation installation;

    explicit
    gh_check_suite_event (json::parser&);

    gh_check_suite_event () = default;
  };

  struct gh_pull_request_event
  {
    string action;

    gh_pull_request pull_request;
    gh_repository repository;
    gh_installation installation;

    explicit
    gh_pull_request_event (json::parser&);

    gh_pull_request_event () = default;
  };

  struct gh_installation_access_token
  {
    string token;
    timestamp expires_at;

    explicit
    gh_installation_access_token (json::parser&);

    gh_installation_access_token (string token, timestamp expires_at);

    gh_installation_access_token () = default;
  };

  string
  gh_to_iso8601 (timestamp);

  timestamp
  gh_from_iso8601 (const string&);

  ostream&
  operator<< (ostream&, const gh_check_suite&);

  ostream&
  operator<< (ostream&, const gh_check_run&);

  ostream&
  operator<< (ostream&, const gh_pull_request&);

  ostream&
  operator<< (ostream&, const gh_repository&);

  ostream&
  operator<< (ostream&, const gh_installation&);

  ostream&
  operator<< (ostream&, const gh_check_suite_event&);

  ostream&
  operator<< (ostream&, const gh_pull_request_event&);

  ostream&
  operator<< (ostream&, const gh_installation_access_token&);
}

#endif // MOD_MOD_CI_GITHUB_GH_HXX
