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
  // Note: no details_url yet since there will be no entry in the build result
  // search page until the task starts building.
  //
  bool
  gq_create_check_runs (const basic_mark& error,
                        vector<check_run>& check_runs,
                        const string& installation_access_token,
                        const string& repository_id,
                        const string& head_sha,
                        build_state);

  // Create a new check run on GitHub for a build. Update `cr` with the new
  // state and the node ID. Return false and issue diagnostics if the request
  // failed.
  //
  // If the details_url is absent GitHub will use the app's homepage.
  //
  // The gq_built_result is required if the build_state is built because
  // GitHub does not allow a check run status of `completed` without at least
  // a conclusion.
  //
  struct gq_built_result
  {
    string conclusion;
    string title;
    string summary;
  };

  bool
  gq_create_check_run (const basic_mark& error,
                       check_run& cr,
                       const string& installation_access_token,
                       const string& repository_id,
                       const string& head_sha,
                       const optional<string>& details_url,
                       build_state,
                       optional<gq_built_result> = nullopt);

  // Update a check run on GitHub.
  //
  // Send a GraphQL request that updates an existing check run. Update `cr`
  // with the new state. Return false and issue diagnostics if the request
  // failed.
  //
  // If the details_url is absent GitHub will use the app's homepage.
  //
  // The gq_built_result is required if the build_state is built because
  // GitHub does not allow a check run status of `completed` without at least
  // a conclusion.
  //
  bool
  gq_update_check_run (const basic_mark& error,
                       check_run& cr,
                       const string& installation_access_token,
                       const string& repository_id,
                       const string& node_id,
                       const optional<string>& details_url,
                       build_state,
                       optional<gq_built_result> = nullopt);

  // Fetch a pull request's mergeability from GitHub and return it in first,
  // or absent if the merge commit is still being generated.
  //
  // Return false in second and issue diagnostics if the request failed.
  //
  struct gq_pr_mergeability
  {
    // True if the pull request is auto-mergeable; false if it would create
    // conflicts.
    //
    bool mergeable;

    // The ID of the test merge commit. Empty if mergeable is false.
    //
    string merge_commit_id;
  };


  // Fetch a pull request's mergeability from GitHub. Return absent value if
  // the merge commit is still being generated. Return empty string if the
  // pull request is not auto-mergeable. Otherwise return the test merge
  // commit id.
  //
  // Issue diagnostics and return absent if the request failed (which means it
  // will be treated by the caller as still being generated).
  //
  // Note that the first request causes GitHub to start preparing the test
  // merge commit.
  //
  optional<string>
  gq_pull_request_mergeable (const basic_mark& error,
                             const string& installation_access_token,
                             const string& node_id);
}

#endif // MOD_MOD_CI_GITHUB_GQ_HXX
