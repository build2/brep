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

  // The check_suite member of a check_run webhook event (gh_check_run_event).
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

  // The check_suite member of a check_suite webhook event
  // (gh_check_suite_event).
  //
  struct gh_check_suite_ex: gh_check_suite
  {
    size_t check_runs_count;
    optional<string> conclusion;

    // Note: unlike the check_run webhook's app_id this can be null.
    //
    optional<uint64_t> app_id;

    explicit
    gh_check_suite_ex (json::parser&);

    gh_check_suite_ex () = default;
  };

  // The check_run object returned in response to GraphQL requests
  // (e.g. create or update check run). Note that we specifiy the set of
  // members to return in the GraphQL request.
  //
  struct gh_check_run
  {
    string node_id;
    string name;
    string status;

    explicit
    gh_check_run (json::parser&);

    gh_check_run () = default;
  };

  // The check_run member of a check_run webhook event (gh_check_run_event).
  //
  struct gh_check_run_ex: gh_check_run
  {
    string details_url;
    gh_check_suite check_suite;

    uint64_t app_id;

    explicit
    gh_check_run_ex (json::parser&);

    gh_check_run_ex () = default;
  };

  // The pull_request member of a pull_request webhook event
  // (gh_pull_request_event).
  //
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

    // Note: not received from GitHub but set from the app-id webhook query
    // parameter instead.
    //
    // For some reason, unlike the check_suite and check_run webhooks, the
    // pull_request webhook does not contain the app id. For the sake of
    // simplicity we emulate check_suite and check_run by storing the app-id
    // webhook query parameter here.
    //
    uint64_t app_id;

    explicit
    gh_pull_request (json::parser&);

    gh_pull_request () = default;
  };

  // The repository member of various webhook events.
  //
  struct gh_repository
  {
    string node_id;
    string path; // Repository path (<org>/<repo>) under github.com.
    string clone_url;

    explicit
    gh_repository (json::parser&);

    gh_repository () = default;
  };

  // The installation member of various webhook events.
  //
  struct gh_installation
  {
    uint64_t id; // Note: used for installation access token (REST API).

    explicit
    gh_installation (json::parser&);

    gh_installation () = default;
  };

  // The check_suite webhook event.
  //
  struct gh_check_suite_event
  {
    string action;
    gh_check_suite_ex check_suite;
    gh_repository repository;
    gh_installation installation;

    explicit
    gh_check_suite_event (json::parser&);

    gh_check_suite_event () = default;
  };

  // The check_run webhook event.
  //
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

  // The pull_request webhook event.
  //
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

  // Installation access token (IAT) returned when we authenticate as a GitHub
  // app installation.
  //
  struct gh_installation_access_token
  {
    string token;
    timestamp expires_at;

    explicit
    gh_installation_access_token (json::parser&);

    gh_installation_access_token (string token, timestamp expires_at);

    gh_installation_access_token () = default;
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

  // If warning_success is true, then map result_status::warning to `SUCCESS`
  // and to `FAILURE` otherwise.
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
  operator<< (ostream&, const gh_check_suite_ex&);

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
