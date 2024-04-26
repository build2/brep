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
                        const build_queued_hints&,
                        const basic_mark& error);

  // Create a new check run on GitHub for a build. Update `cr` with the new
  // state and the node ID. Return false and issue diagnostics if the request
  // failed.
  //
  // The result_status is required if the build_state is built because GitHub
  // does not allow a check run status of `completed` without a conclusion.
  //
  // @@ TODO Support output (title, summary, text).
  //
  bool
  gq_create_check_run (check_run& cr,
                       const string& installation_access_token,
                       const string& repository_id,
                       const string& head_sha,
                       const build&,
                       build_state,
                       optional<result_status>,
                       const build_queued_hints&,
                       const basic_mark& error);

  // Update a check run on GitHub.
  //
  // Send a GraphQL request that updates an existing check run. Update `cr`
  // with the new state. Return false and issue diagnostics if the request
  // failed.
  //
  // The result_status is required if the build_state is built because GitHub
  // does not allow updating a check run to `completed` without a conclusion.
  //
  // @@ TODO Support output (title, summary, text).
  //
  bool
  gq_update_check_run (check_run& cr,
                       const string& installation_access_token,
                       const string& repository_id,
                       const string& node_id,
                       build_state,
                       optional<result_status>,
                       const basic_mark& error);
}

#endif // MOD_MOD_CI_GITHUB_GQ_HXX
