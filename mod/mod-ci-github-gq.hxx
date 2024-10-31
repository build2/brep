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
  // Note that creating a check_run named `foo` will effectively replace any
  // existing check_runs with that name. They will still exist on the GitHub
  // servers but GitHub will only consider the latest one (for display in the
  // UI or in determining the mergeability of a PR).
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

  // Fetch pre-check information for a pull request from GitHub. This
  // information is used to decide whether or not to CI the PR and is
  // comprised of the PR's head commit SHA, whether its head branch is behind
  // its base branch, and its mergeability and test merge commit SHA.
  //
  // Return absent value if the merge commit is still being generated (which
  // means PR head branch behindness is not yet known either). See the
  // gq_pr_pre_check struct's member comments for non-absent return value
  // semantics.
  //
  // Issue diagnostics and return absent if the request failed (which means it
  // will be treated by the caller as still being generated).
  //
  // Note that the first request causes GitHub to start preparing the test
  // merge commit.
  //
  // For details regarding the test merge commit and how to check/poll for PR
  // mergeability see
  // https://docs.github.com/en/rest/pulls/pulls?#get-a-pull-request and
  // https://docs.github.com/en/rest/guides/using-the-rest-api-to-interact-with-your-git-database?#checking-mergeability-of-pull-requests
  //
  struct gq_pr_pre_check_info
  {
    // The PR head commit id.
    //
    string head_sha;

    // True if the PR's head branch is behind its base branch.
    //
    bool behind;

    // The commit id of the test merge commit. Absent if behind or the PR is
    // not auto-mergeable.
    //
    optional<string> merge_commit_sha;
  };

  optional<gq_pr_pre_check_info>
  gq_fetch_pull_request_pre_check_info (
    const basic_mark& error,
    const string& installation_access_token,
    const string& node_id);
}

#endif // MOD_MOD_CI_GITHUB_GQ_HXX
