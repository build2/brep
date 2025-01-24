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

  // Create a new check run on GitHub for each build with the build state,
  // name, details_url, and output taken from each check_run object. Update
  // `check_runs` with the new data (node id and state_synced). Return false
  // and issue diagnostics if the request failed.
  //
  // Throw invalid_argument if the passed data is invalid, missing, or
  // inconsistent.
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
                        const string& head_sha);

  // Create a new check run on GitHub for a build in the queued or building
  // state. Note that the state cannot be built because in that case a
  // conclusion is required.
  //
  // Update `cr` with the new data (node id, state, and state_synced). Return
  // false and issue diagnostics if the request failed.
  //
  // Throw invalid_argument if the passed data is invalid, missing, or
  // inconsistent.
  //
  // If the details_url is absent GitHub will use the app's homepage. Title
  // and summary are required and cannot be empty.
  //
  bool
  gq_create_check_run (const basic_mark& error,
                       check_run& cr,
                       const string& installation_access_token,
                       const string& repository_id,
                       const string& head_sha,
                       const optional<string>& details_url,
                       build_state,
                       string title,
                       string summary);

  // As above but create a check run in the built state (which requires a
  // conclusion).
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
                       gq_built_result);

  // Update a check run on GitHub to the queued or building state. Note that
  // the state cannot be built because in that case a conclusion is required.
  //
  // Update `cr` with the new data (state and state_synced). Return false and
  // issue diagnostics if the request failed.
  //
  // Throw invalid_argument if the passed data is invalid, missing, or
  // inconsistent.
  //
  // Title and summary are required and cannot be empty.
  //
  bool
  gq_update_check_run (const basic_mark& error,
                       check_run& cr,
                       const string& installation_access_token,
                       const string& repository_id,
                       const string& node_id,
                       build_state,
                       string title,
                       string summary);

  // As above but update a check run to the built state (which requires a
  // conclusion).
  //
  // Note that GitHub allows any state transitions except from built (but
  // built to built is allowed). The latter case is signalled by setting the
  // check_run state_synced member to false and the state member to built.
  //
  bool
  gq_update_check_run (const basic_mark& error,
                       check_run& cr,
                       const string& installation_access_token,
                       const string& repository_id,
                       const string& node_id,
                       gq_built_result);

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
  // Throw invalid_argument if the node id is invalid.
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
