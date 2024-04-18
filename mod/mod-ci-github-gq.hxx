// file      : mod/mod-ci-github-gq.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_CI_GITHUB_GQ_HXX
#define MOD_MOD_CI_GITHUB_GQ_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <libbrep/build.hxx>

#include <mod/tenant-service.hxx> // build_hints

#include <mod/mod-ci-github-gh.hxx>
#include <mod/mod-ci-github-service-data.hxx>

namespace brep
{
  // GraphQL functions (all start with gq_).
  //

  // Create a new check run on GitHub for each build. Update `check_runs` with
  // the new states and node IDs. Return false and issue diagnostics if the
  // request failed.
  //
  bool
  gq_create_check_runs (vector<check_run>& check_runs,
                        const string& installation_access_token,
                        const string& repository_id,
                        const string& head_sha,
                        const vector<reference_wrapper<const build>>&,
                        build_state,
                        const tenant_service_base::build_hints&,
                        const basic_mark& error);

  // Create a new check run on GitHub for a build. Update `cr` with the new
  // state and the node ID. Return false and issue diagnostics if the request
  // failed.
  //
  bool
  gq_create_check_run (check_run& cr,
                       const string& installation_access_token,
                       const string& repository_id,
                       const string& head_sha,
                       const build&,
                       build_state,
                       const tenant_service_base::build_hints&,
                       const basic_mark& error);

  // Update a check run on GitHub.
  //
  // Send a GraphQL request that updates an existing check run. Update `cr`
  // with the new state. Return false and issue diagnostics if the request
  // failed.
  //
  bool
  gq_update_check_run (check_run& cr,
                       const string& installation_access_token,
                       const string& repository_id,
                       const string& node_id,
                       build_state,
                       const basic_mark& error);

  // Fetch from GitHub the check run with the specified name (hints-shortened
  // build ID).
  //
  // Return the check run or nullopt if no such check run exists.
  //
  // In case of error diagnostics will be issued and false returned in second.
  //
  // Note that the existence of more than one check run with the same name is
  // considered an error and reported as such. The API docs imply that there
  // can be more than one check run with the same name in a check suite, but
  // the observed behavior is that creating a check run destroys the existent
  // one, leaving only the new one with a different node ID.
  //
  pair<optional<gh_check_run>, bool>
  gq_fetch_check_run (const string& installation_access_token,
                      const string& check_suite_id,
                      const string& cr_name,
                      const basic_mark& error) noexcept;
}

#endif // MOD_MOD_CI_GITHUB_GQ_HXX
