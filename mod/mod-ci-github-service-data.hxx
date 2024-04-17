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
    string                build_id; // Full build id.
    optional<string>      node_id;  // GitHub id.

    // @@ TODO
    //
    // build_state           state;
    // bool                  state_synced;

    // string
    // state_string () const
    // {
    //   string r (to_string (*state));
    //   if (!state_synced)
    //     r += "(unsynchronized)";
    //   return r;
    // }

    optional<build_state> state;

    string
    state_string () const
    {
      return state ? to_string (*state) : "null";
    }
  };

  struct service_data
  {
    // The data schema version. Note: must be first member in the object.
    //
    uint64_t version = 1;

    // Check suite-global data.
    //
    gh_installation_access_token installation_access;

    uint64_t installation_id;
    // @@ TODO Rename to repository_node_id.
    //
    string repository_id; // GitHub-internal opaque repository id.

    string head_sha;

    vector<check_run> check_runs;

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

    service_data (string iat_token,
                  timestamp iat_expires_at,
                  uint64_t installation_id,
                  string repository_id,
                  string head_sha);

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
