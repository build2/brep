// file      : mod/mod-ci-github-service-data.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_CI_GITHUB_SERVICE_DATA_HXX
#define MOD_MOD_CI_GITHUB_SERVICE_DATA_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/mod-ci-github-gh.hxx>

namespace brep
{
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

    optional<result_status> status; // Only present if state is built.

    string
    state_string () const
    {
      string r (to_string (state));
      if (!state_synced)
        r += "(unsynchronized)";
      return r;
    }
  };

  struct service_data
  {
    // The data schema version. Note: must be first member in the object.
    //
    uint64_t version = 1;

    // Check suite settings.
    //
    bool warning_success; // See gh_to_conclusion().

    // Check suite-global data.
    //
    gh_installation_access_token installation_access;

    uint64_t installation_id;

    string repository_node_id; // GitHub-internal opaque repository id.

    // The following two are only used for pull requests.
    //
    // @@ TODO/LATER: maybe put them in a struct?
    //
    optional<string> repository_clone_url;
    optional<uint32_t> pr_number;

    // The GitHub ID of the synthetic PR merge check run or absent if it
    // hasn't been created yet.
    //
    optional<string> merge_node_id;

    // The commit ID the check suite or pull request (and its check runs) are
    // reporting to. Note that in the case of a pull request this will be the
    // head commit (`pull_request.head.sha`) as opposed to the merge commit.
    //
    string report_sha;

    vector<check_run> check_runs;

    // The GitHub ID of the synthetic conclusion check run or absent if it
    // hasn't been created yet. See also merge_node_id above.
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
    explicit
    service_data (const string& json);

    // The check_suite constructor.
    //
    service_data (bool warning_success,
                  string iat_token,
                  timestamp iat_expires_at,
                  uint64_t installation_id,
                  string repository_node_id,
                  string report_sha);

    // The pull_request constructor.
    //
    service_data (bool warning_success,
                  string iat_token,
                  timestamp iat_expires_at,
                  uint64_t installation_id,
                  string repository_node_id,
                  string report_sha,
                  string repository_clone_url,
                  uint32_t pr_number);

    service_data () = default;

    // Serialize to JSON.
    //
    string
    json () const;
  };

  ostream&
  operator<< (ostream&, const check_run&);
}

#endif // MOD_MOD_CI_GITHUB_SERVICE_DATA_HXX
