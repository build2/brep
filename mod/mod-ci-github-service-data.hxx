// file      : mod/mod-ci-github-service-data.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_CI_GITHUB_SERVICE_DATA_HXX
#define MOD_MOD_CI_GITHUB_SERVICE_DATA_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/mod-ci-github-gh.hxx>

namespace brep
{
  // @@@ Check if any data members are unused (once the dust settles).

  // Service data associated with the tenant (corresponds to GH check suite).
  //
  // It is always a top-level JSON object and the first member is always the
  // schema version.

  // Unsynchronized state means we were unable to (conclusively) notify
  // GitHub about the last state transition (e.g., due to a transient
  // network error). The "conclusively" part means that the notification may
  // or may not have gone through. Note: node_id can be absent for the same
  // reason.
  //
  struct check_run
  {
    string                  build_id; // Full build id.
    string                  name;     // Potentially shortened build id.
    optional<string>        node_id;  // GitHub id.

    build_state             state;
    bool                    state_synced;

    optional<result_status> status; // Only if state is built & synced.

    // Note: never serialized (only used to pass information to the GraphQL
    // functions).
    //
    optional<string>        details_url;

    string
    state_string () const
    {
      string r (to_string (state));
      if (!state_synced)
        r += "(unsynchronized)";
      return r;
    }
  };

  // We have two kinds of service data that correspond to the following two
  // typical scenarios (until/unless we add support for merge queues):
  //
  // 1. Branch push (via check_suite) plus zero or more local PRs (via
  //    pull_request) that share the same head commit id.
  //
  // 2. One or more remote PRs (via pull_request) that share the same head
  //    commit id (from a repository in another organization).
  //
  // Plus, for PRs, the service data may be in the pre-check phase while we
  // are in the process of requesting the test merge commit and making sure it
  // can be created and is not behind base. We do all this before we actually
  // create the CI tenant.
  //
  // Note that the above two cases are typical but not the only possible
  // scenarios. Specifically, it is possible to have a mixture of all three
  // kinds (branch push, local PR, and remote PR) since the same head commit
  // id can be present in both local and remote branches. There is no way to
  // handle this case perfectly and we do the best we can (see
  // build_unloaded_pre_check() for details).
  //
  struct service_data
  {
    // The data schema version. Note: must be first member in the object.
    //
    uint64_t version = 1;

    // Kind and phase.
    //
    enum kind_type {local, remote /*, queue */} kind;
    bool pre_check;
    bool re_request; // Re-requested (rebuild).

    // Check suite settings.
    //
    bool warning_success; // See gh_to_conclusion().

    // Check suite-global data.
    //
    gh_installation_access_token installation_access;

    uint64_t installation_id;

    string repository_node_id; // GitHub-internal opaque repository id.

    string repository_clone_url;

    // The following two are only used for pull requests.
    //
    // @@ TODO/LATER: maybe put them in a struct?
    //
    optional<string>   pr_node_id;
    optional<uint32_t> pr_number;

    // The commit ID the branch push or pull request (and its check runs) are
    // building. This will be the head commit for the branch push as well as
    // local pull requests and the test merge commit for remote pull requests.
    //
    string check_sha;

    // The commit ID the branch push or pull request (and its check runs) are
    // reporting to. Note that in the case of a pull request this will be the
    // head commit (`pull_request.head.sha`) as opposed to the test merge
    // commit.
    //
    string report_sha;

    vector<check_run> check_runs;

    // Flag indicating that all the elements in check_runs are built and this
    // check suite is completed.
    //
    bool completed;

    // The GitHub ID of the synthetic conclusion check run or absent if it
    // hasn't been created yet.
    //
    optional<string> conclusion_node_id;

    // Return the check run with the specified build ID or nullptr if not
    // found.
    //
    check_run*
    find_check_run (const string& build_id);

    // Construct from JSON.
    //
    // Throw invalid_argument if the schema version is not supported.
    //
    // Throw invalid_argument (invalid_json_input) in case of malformed JSON
    // or any invalid values.
    //
    explicit
    service_data (const string& json);

    // The check_suite constructor.
    //
    // Note that check_sha and report_sha are both the SHA of the
    // check_suite's head commit.
    //
    service_data (bool warning_success,
                  string iat_token,
                  timestamp iat_expires_at,
                  uint64_t installation_id,
                  string repository_node_id,
                  string repository_clone_url,
                  kind_type kind,
                  bool pre_check,
                  bool re_request,
                  string check_sha,
                  string report_sha);

    // The pull_request constructor.
    //
    service_data (bool warning_success,
                  string iat_token,
                  timestamp iat_expires_at,
                  uint64_t installation_id,
                  string repository_node_id,
                  string repository_clone_url,
                  kind_type kind,
                  bool pre_check,
                  bool re_request,
                  string check_sha,
                  string report_sha,
                  string pr_node_id,
                  uint32_t pr_number);

    service_data () = default;

    // Serialize to JSON.
    //
    // Throw invalid_argument if any values are invalid.
    //
    // May also throw invalid_json_output but that would be a programming
    // error.
    //
    string
    json () const;
  };

  ostream&
  operator<< (ostream&, const check_run&);
}

#endif // MOD_MOD_CI_GITHUB_SERVICE_DATA_HXX
