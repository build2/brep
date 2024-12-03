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
  // Note that the GitHub REST and GraphQL APIs use different id types and
  // values. In the REST API they are usually integers (but sometimes
  // strings!) whereas in GraphQL they are always strings (note:
  // base64-encoded and opaque, not just the REST id value as a string).
  //
  // In both APIs the id field is called `id`, but REST responses and webhook
  // events also contain the corresponding GraphQL object's id in the
  // `node_id` field.
  //
  // The GraphQL API's ids are called "global node ids" by GitHub. We refer to
  // them simply as node ids and we use them almost exclusively (over the
  // REST/webhook ids).
  //
  // In the structures below, `id` always refers to the REST/webhook id and
  // `node_id` always refers to the node id.
  //
  namespace json = butl::json;

  // The "check_suite" object within a check_suite webhook event request.
  //
  struct gh_check_suite
  {
    string node_id;
    optional<string> head_branch;
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

  struct gh_check_run_ex: gh_check_run
  {
    string details_url;
    gh_check_suite check_suite;

    explicit
    gh_check_run_ex (json::parser&);

    gh_check_run_ex () = default;
  };

  struct gh_pull_request
  {
    string node_id;
    unsigned int number;

    string base_path; // Repository path (<org>/<repo>) under github.com.
    string base_ref;
    string base_sha;

    string head_path;
    string head_ref;
    string head_sha;

    explicit
    gh_pull_request (json::parser&);

    gh_pull_request () = default;
  };

  // Return the GitHub check run status corresponding to a build_state.
  //
  string
  gh_to_status (build_state);

  // Return the build_state corresponding to a GitHub check run status
  // string. Throw invalid_argument if the passed status was invalid.
  //
  build_state
  gh_from_status (const string&);

  // If warning_success is true, then map result_status::warning to SUCCESS
  // and to FAILURE otherwise.
  //
  // Throw invalid_argument in case of unsupported result_status value
  // (currently skip, interrupt).
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
    string path; // Repository path (<org>/<repo>) under github.com.
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

  struct gh_check_run_event
  {
    string action;
    gh_check_run_ex check_run;
    gh_repository repository;
    gh_installation installation;

    explicit
    gh_check_run_event (json::parser&);

    gh_check_run_event () = default;
  };

  struct gh_pull_request_event
  {
    string action;

    gh_pull_request pull_request;

    // The SHA of the previous commit on the head branch before the current
    // one. Only present if action is "synchronize".
    //
    optional<string> before;

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

  // Throw system_error if the conversion fails due to underlying operating
  // system errors.
  //
  string
  gh_to_iso8601 (timestamp);

  // Throw invalid_argument if the conversion fails due to the invalid
  // argument and system_error if due to underlying operating system errors.
  //
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
  operator<< (ostream&, const gh_check_run_event&);

  ostream&
  operator<< (ostream&, const gh_pull_request_event&);

  ostream&
  operator<< (ostream&, const gh_installation_access_token&);
}

#endif // MOD_MOD_CI_GITHUB_GH_HXX
