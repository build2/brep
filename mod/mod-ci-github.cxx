// file      : mod/mod-ci-github.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-ci-github.hxx>

#include <libbutl/json/parser.hxx>

#include <web/xhtml/serialization.hxx>
#include <web/server/mime-url-encoding.hxx> // mime_url_encode()

#include <mod/jwt.hxx>
#include <mod/hmac.hxx>
#include <mod/build.hxx> // build_log_url()
#include <mod/module-options.hxx>

#include <mod/mod-ci-github-gq.hxx>
#include <mod/mod-ci-github-post.hxx>
#include <mod/mod-ci-github-service-data.hxx>

#include <stdexcept>

// @@ Remaining TODOs
//
//    - Rerequested checks
//
//      - check_suite (action: rerequested): received when user re-runs all
//        checks.
//
//      - check_run (action: rerequested): received when user re-runs a
//        specific check or all failed checks.
//
//        @@ TMP I have confirmed that the above is accurate.
//
//      Will need to extract a few more fields from check_runs, but the layout
//      is very similar to that of check_suite.
//
//    - Choose strong webhook secret (when deploying).
//
//    - Check that delivery UUID has not been received before (replay attack).
//

// Resources:
//
//    Creating an App:
//    https://docs.github.com/en/apps/creating-github-apps/about-creating-github-apps/best-practices-for-creating-a-github-app
//
//    Webhooks:
//    https://docs.github.com/en/webhooks/using-webhooks/best-practices-for-using-webhooks
//    https://docs.github.com/en/webhooks/using-webhooks/validating-webhook-deliveries
//
//    REST API:
//    All docs:       https://docs.github.com/en/rest#all-docs
//    Best practices: https://docs.github.com/en/rest/using-the-rest-api/best-practices-for-using-the-rest-api
//
//    GraphQL API:
//    Reference: https://docs.github.com/en/graphql/reference
//

using namespace std;
using namespace butl;
using namespace web;
using namespace brep::cli;

namespace brep
{
  ci_github::
  ci_github (tenant_service_map& tsm)
      : tenant_service_map_ (tsm)
  {
  }

  ci_github::
  ci_github (const ci_github& r, tenant_service_map& tsm)
      : database_module (r),
        ci_start (r),
        options_ (r.initialized_ ? r.options_ : nullptr),
        tenant_service_map_ (tsm)
  {
  }

  void ci_github::
  init (scanner& s)
  {
    {
      shared_ptr<tenant_service_base> ts (
        dynamic_pointer_cast<tenant_service_base> (shared_from_this ()));

      assert (ts != nullptr); // By definition.

      tenant_service_map_["ci-github"] = move (ts);
    }

    options_ = make_shared<options::ci_github> (
      s, unknown_mode::fail, unknown_mode::fail);

    // Prepare for the CI requests handling, if configured.
    //
    if (options_->build_config_specified () &&
        options_->ci_github_app_webhook_secret_specified ())
    {
      ci_start::init (make_shared<options::ci_start> (*options_));

      database_module::init (*options_, options_->build_db_retry ());
    }
  }

  bool ci_github::
  handle (request& rq, response&)
  {
    using namespace bpkg;

    HANDLER_DIAG;

    if (build_db_ == nullptr)
      throw invalid_request (501, "GitHub CI submission not implemented");

    // Process headers.
    //
    string event; // Webhook event.
    string hmac;  // Received HMAC.
    try
    {
      bool content_type (false);

      for (const name_value& h: rq.headers ())
      {
        // HMAC authenticating this request. Note that it won't be present
        // unless a webhook secret has been set in the GitHub app's settings.
        //
        if (icasecmp (h.name, "x-hub-signature-256") == 0)
        {
          if (!h.value)
            throw invalid_request (400, "missing x-hub-signature-256 value");

          // Parse the x-hub-signature-256 header value. For example:
          //
          // sha256=5e82258...
          //
          // Check for the presence of the "sha256=" prefix and then strip it
          // to leave only the HMAC value.
          //
          if (h.value->find ("sha256=", 0, 7) == string::npos)
            throw invalid_request (400, "invalid x-hub-signature-256 value");

          hmac = h.value->substr (7);
        }
        // This event's UUID.
        //
        else if (icasecmp (h.name, "x-github-delivery") == 0)
        {
          // @@ TODO Check that delivery UUID has not been received before
          //         (replay attack).
        }
        else if (icasecmp (h.name, "content-type") == 0)
        {
          if (!h.value)
            throw invalid_request (400, "missing content-type value");

          if (icasecmp (*h.value, "application/json") != 0)
          {
            throw invalid_request (400,
                                   "invalid content-type value: '" + *h.value +
                                   '\'');
          }

          content_type = true;
        }
        // The webhook event.
        //
        else if (icasecmp (h.name, "x-github-event") == 0)
        {
          if (!h.value)
            throw invalid_request (400, "missing x-github-event value");

          event = *h.value;
        }
      }

      if (!content_type)
        throw invalid_request (400, "missing content-type header");

      if (event.empty ())
        throw invalid_request (400, "missing x-github-event header");

      if (hmac.empty ())
        throw invalid_request (400, "missing x-hub-signature-256 header");
    }
    catch (const invalid_request& e)
    {
      error << "request header error: " << e.content;
      throw;
    }

    // Read the entire request body into a buffer because we need to compute
    // an HMAC over it and then parse it as JSON. The alternative of reading
    // from the stream twice works out to be more complicated (see also a TODO
    // item in web/server/module.hxx).
    //
    string body;
    {
      // Note that even though we may not need caching right now, we may later
      // (e.g., to support cancel) so let's just enable it right away.
      //
      size_t limit (128 * 1024);

      istream& is (rq.content (limit, limit));

      try
      {
        getline (is, body, '\0');
      }
      catch (const io_error& e)
      {
        fail << "unable to read request body: " << e;
      }
    }

    // Verify the received HMAC.
    //
    // Compute the HMAC value over the request body using the configured
    // webhook secret as key and compare it to the received HMAC.
    //
    try
    {
      string h (
        compute_hmac (*options_,
                      body.data (), body.size (),
                      options_->ci_github_app_webhook_secret ().c_str ()));

      if (!icasecmp (h, hmac))
      {
        string m ("computed HMAC does not match received HMAC");

        error << m;

        throw invalid_request (400, move (m));
      }
    }
    catch (const system_error& e)
    {
      fail << "unable to compute request HMAC: " << e;
    }

    // Process the `warning` webhook request query parameter.
    //
    bool warning_success;
    {
      const name_values& rps (rq.parameters (1024, true /* url_only */));

      auto i (find_if (rps.begin (), rps.end (),
                       [] (auto&& rp) {return rp.name == "warning";}));

      if (i == rps.end ())
        throw invalid_request (400,
                               "missing 'warning' webhook query parameter");

      if (!i->value)
        throw invalid_request (
          400, "missing 'warning' webhook query parameter value");

      const string& v (*i->value);

      if      (v == "success") warning_success = true;
      else if (v == "failure") warning_success = false;
      else
      {
        throw invalid_request (
            400,
            "invalid 'warning' webhook query parameter value: '" + v + '\'');
      }
    }

    // There is a webhook event (specified in the x-github-event header) and
    // each event contains a bunch of actions (specified in the JSON request
    // body).
    //
    // Note: "GitHub continues to add new event types and new actions to
    // existing event types." As a result we ignore known actions that we are
    // not interested in and log and ignore unknown actions. The thinking here
    // is that we want be "notified" of new actions at which point we can
    // decide whether to ignore them or to handle.
    //
    // @@ There is also check_run even (re-requested by user, either
    //    individual check run or all the failed check runs).
    //
    if (event == "check_suite")
    {
      gh_check_suite_event cs;
      try
      {
        json::parser p (body.data (), body.size (), "check_suite event");

        cs = gh_check_suite_event (p);
      }
      catch (const json::invalid_json_input& e)
      {
        string m ("malformed JSON in " + e.name + " request body");

        error << m << ", line: " << e.line << ", column: " << e.column
              << ", byte offset: " << e.position << ", error: " << e;

        throw invalid_request (400, move (m));
      }

      if (cs.action == "requested")
      {
        return handle_check_suite_request (move (cs), warning_success);
      }
      else if (cs.action == "rerequested")
      {
        // Someone manually requested to re-run all the check runs in this
        // check suite. Treat as a new request.
        //
        return handle_check_suite_request (move (cs), warning_success);
      }
      else if (cs.action == "completed")
      {
        // GitHub thinks that "all the check runs in this check suite have
        // completed and a conclusion is available". Looks like this one we
        // ignore?
        //
        // What if our bookkeeping says otherwise? But then we can't even
        // access the service data easily here. @@ TODO: maybe/later.
        //
        return true;
      }
      else
      {
        // Ignore unknown actions by sending a 200 response with empty body
        // but also log as an error since we want to notice new actions.
        //
        error << "unknown action '" << cs.action << "' in check_suite event";

        return true;
      }
    }
    else if (event == "pull_request")
    {
      gh_pull_request_event pr;
      try
      {
        json::parser p (body.data (), body.size (), "pull_request event");

        pr = gh_pull_request_event (p);
      }
      catch (const json::invalid_json_input& e)
      {
        string m ("malformed JSON in " + e.name + " request body");

        error << m << ", line: " << e.line << ", column: " << e.column
              << ", byte offset: " << e.position << ", error: " << e;

        throw invalid_request (400, move (m));
      }

      if (pr.action == "opened" || pr.action == "synchronize")
      {
        // opened
        //   A pull request was opened.
        //
        // synchronize
        //   A pull request's head branch was updated from the base branch or
        //   new commits were pushed to the head branch. (Note that there is
        //   no equivalent event for the base branch. That case gets handled
        //   in handle_check_suite_request() instead. @@ Not anymore.)
        //
        // Note that both cases are handled the same: we start a new CI
        // request which will be reported on the new commit id.
        //
        return handle_pull_request (move (pr), warning_success);
      }
      else
      {
        // Ignore the remaining actions by sending a 200 response with empty
        // body.
        //
        // @@ Ignore known but log unknown, as in check_suite above?
        //
        return true;
      }
    }
    else
    {
      // Log to investigate.
      //
      error << "unexpected event '" << event << "'";

      throw invalid_request (400, "unexpected event: '" + event + "'");
    }
  }

  // Let's capitalize the synthetic conclusion check run name to make it
  // easier to distinguish from the regular ones.
  //
  static string conclusion_check_run_name ("CONCLUSION");

  // Return the colored circle corresponding to a result_status.
  //
  static string
  circle (result_status rs)
  {
    switch (rs)
    {
    case result_status::success:  return "\U0001F7E2"; // Green circle.
    case result_status::warning:  return "\U0001F7E0"; // Orange circle.
    case result_status::error:
    case result_status::abort:
    case result_status::abnormal: return "\U0001F534"; // Red circle.

      // Valid values we should never encounter.
      //
    case result_status::skip:
    case result_status::interrupt:
      throw invalid_argument ("unexpected result_status value: " +
                              to_string (rs));
    }

    return ""; // Should never reach.
  }

  // Make a check run summary from a CI start_result.
  //
  static string
  to_check_run_summary (const optional<ci_start::start_result>& r)
  {
    string s;

    s = "```\n";
    if (r) s += r->message;
    else s += "Internal service error";
    s += "\n```";

    return s;
  }

  bool ci_github::
  handle_check_suite_request (gh_check_suite_event cs, bool warning_success)
  {
    HANDLER_DIAG;

    l3 ([&]{trace << "check_suite event { " << cs << " }";});

    // While we don't need the installation access token in this request,
    // let's obtain it to flush out any permission issues early. Also, it is
    // valid for an hour so we will most likely make use of it.
    //
    optional<string> jwt (generate_jwt (trace, error));
    if (!jwt)
      throw server_error ();

    optional<gh_installation_access_token> iat (
        obtain_installation_access_token (cs.installation.id,
                                          move (*jwt),
                                          error));
    if (!iat)
      throw server_error ();

    l3 ([&]{trace << "installation_access_token { " << *iat << " }";});

    // @@ What happens if we call this functions with an already existing
    //    node_id (e.g., replay attack). See the UUID header above.
    //

    // While it would have been nice to cancel CIs of PRs with this branch as
    // base not to waste resources, there are complications: Firsty, we can
    // only do this for remote PRs (since local PRs may share the result with
    // branch push). Secondly, we try to do our best even if the branch
    // protection rule for head behind is not enabled. In this case, it would
    // be good to complete the CI. So maybe/later.

    // Service id that uniquely identifies the CI tenant.
    //
    string sid (cs.repository.node_id + ":" + cs.check_suite.head_sha);

    // If the user requests a rebuild of the (entire) PR, then this manifests
    // as the check_suite rather than pull_request event. Specifically:
    //
    // - For a local PR, this event is shared with the branch push and all we
    //   need to do is restart the CI for the head commit.
    //
    // - For a remote PR, this event will have no gh_check_suite::head_branch.
    //   In this case we need to load the existing service data for this head
    //   commit, extract the test merge commit, and restart the CI for that.
    //
    //   Note that it's possible the base branch has moved in the meantime and
    //   ideally we would want to re-request the test merge commit, etc.
    //   However, this will only be necessary if the user does not follow our
    //   recommendation of enabling the head-behind-base protection. And it
    //   seems all this extra complexity would not be warranted.
    //
    string check_sha;
    service_data::kind_type kind;

    bool re_requested (cs.action == "rerequested");
    if (re_requested && !cs.check_suite.head_branch)
    {
      kind = service_data::remote;

      if (optional<tenant_service> ts = find (*build_db_, "ci-github", sid))
      {
        try
        {
          service_data sd (*ts->data);
          check_sha = move (sd.check_sha); // Test merge commit.
        }
        catch (const invalid_argument& e)
        {
          fail << "failed to parse service data: " << e;
        }
      }
      else
      {
        error << "check suite " << cs.check_suite.node_id
              << " for remote pull request:"
              << " re-requested but tenant_service with id " << sid
              << " did not exist";
        return true;
      }
    }
    else
    {
      // Branch push or local PR rebuild.
      //
      kind = service_data::local;
      check_sha = cs.check_suite.head_sha;
    }

    service_data sd (warning_success,
                     iat->token,
                     iat->expires_at,
                     cs.installation.id,
                     move (cs.repository.node_id),
                     move (cs.repository.clone_url),
                     kind, false /* pre_check */, re_requested,
                     move (check_sha),
                     move (cs.check_suite.head_sha) /* report_sha */);

    // If this check suite is being re-run, replace the existing CI tenant if
    // it exists; otherwise create a new one, doing nothing if a tenant
    // already exists (which could've been created by handle_pull_request()).
    //
    // Note that GitHub UI does not allow re-running the entire check suite
    // until all the check runs are completed.
    //
    duplicate_tenant_mode dtm (re_requested
                               ? duplicate_tenant_mode::replace
                               : duplicate_tenant_mode::ignore);

    // Create an unloaded CI tenant.
    //
    // Note: use no delay since we need to (re)create the synthetic conclusion
    // check run as soon as possible.
    //
    // Note that we use the create() API instead of start() since duplicate
    // management is not available in start().
    //
    // After this call we will start getting the build_unloaded()
    // notifications until (1) we load the tenant, (2) we cancel it, or (3)
    // it gets archived after some timeout.
    //
    auto pr (create (error,
                     warn,
                     verb_ ? &trace : nullptr,
                     *build_db_,
                     tenant_service (sid, "ci-github", sd.json ()),
                     chrono::seconds (30) /* interval */,
                     chrono::seconds (0) /* delay */,
                     dtm));

    if (!pr)
    {
      fail << "check suite " << cs.check_suite.node_id
           << ": unable to create unloaded CI tenant";
    }

    if (dtm == duplicate_tenant_mode::replace &&
        pr->second == duplicate_tenant_result::created)
    {
      error << "check suite " << cs.check_suite.node_id
            << ": re-requested but tenant_service with id " << sid
            << " did not exist";
      return true;
    }

    return true;
  }

  // High-level description of pull request (PR) handling
  //
  // @@ TODO Update these comments.
  //
  // - Some GitHub pull request terminology:
  //
  //   - Fork and pull model: Pull requests are created in a forked
  //     repository. Thus the head and base repositories are different.
  //
  //   - Shared repository model: The pull request head and base branches are
  //     in the same repository. For example, from a feature branch onto
  //     master.
  //
  // - CI the merge commit but add check runs to the pull request head commit
  //
  //   Most of the major CI integrations build the merge commit instead of the
  //   PR head commit.
  //
  //   Adding the check runs to the PR head commit is recommended by the
  //   following blog posts by a GitHub employee who is one of the best
  //   sources on these topics:
  //   https://www.kenmuse.com/blog/shared-commits-and-github-checks/ and
  //   https://www.kenmuse.com/blog/creating-github-checks/.
  //
  //   Do not add any check runs to the merge commit because:
  //
  //   - The PR head commit is the only commit that can be relied upon to
  //     exist throughout the PR's lifetime. The merge commit, on the other
  //     hand, can change during the PR process. When that happens the PR will
  //     look for check runs on the new merge commit, effectively discarding
  //     the ones we had before.
  //
  //   - Although some of the GitHub documentation makes it sound like they
  //     expect check runs to be added to both the PR head commit and the
  //     merge commit, the PR UI does not react to the merge commit's check
  //     runs consistently. It actually seems to be quite broken.
  //
  //     The only thing it seems to do reliably is blocking the PR merge if
  //     the merge commit's check runs are not successful (i.e, overriding the
  //     PR head commit's check runs). But the UI looks quite messed up
  //     generally in this state.
  //
  //   Note that, in the case of a PR from a forked repository (the so-called
  //   "fork and pull" model), GitHub will copy the PR head commit from the
  //   head repository (the forked one) into the base repository. So the check
  //   runs must always be added to the base repository, whether the PR is
  //   from within the same repository or from a forked repository. The merge
  //   and head commits will be at refs/pull/<PR-number>/{merge,head}.
  //
  // - @@ TODO Shared repo model problems
  //
  //      The root of all of these problems is that, in the shared repository
  //      model, for every PR we also receive a check_suite for the commit to
  //      the head branch. The situation is exacerbated by the fact that the
  //      PR and CS can arrive in any order.
  //
  //       - There will be two CIs running concurrently, building different
  //         commits: the head commit (check_suite) vs merge commit
  //         (pull_request).
  //
  //       - The CS and PR check_runs will all be added to the same commit
  //         SHA: pull_request.head.sha, check_suite.head_sha (see above for
  //         the reasons we don't put the check_runs on the merge
  //         commit).
  //
  //       - Recall that creating a check_run named `A` will effectively
  //         replace any existing check_runs with that name. They will still
  //         exist on the GitHub servers but GitHub will only consider the
  //         latest one (for display in the UI or in determining the
  //         mergeability of a PR).
  //
  //       - Some CS CRs may finish after the corresponding PR CRs, thus
  //         potentially inverting the true status of a PR (e.g., allow the
  //         merge of a PR with a bad merge commit).
  //
  //   Thus we need a way to prevent any CS check_runs from being updated
  //   after having received a PR.
  //
  //   Problem 1: Create PR from feature branch
  //
  //     - Receive check_suite for commit to feature branch
  //     - Receive pull_request
  //
  //     Solution: When receive a PR, fetch all check_suites with that head
  //     SHA (curl REPO/commits/SHA/check-suites) and cancel their CI
  //     jobs.
  //
  //     Thus there will be no more CS check_run updates. Note however that in
  //     most cases the PR will be received long enough after the CS for the
  //     latter's check_runs to all have completed already.
  //
  //     Note that there will not be a merge CR on the head yet so the PR will
  //     never be green.
  //
  //   Problem 2: New commits are added to PR head branch
  //
  //     Note: The check_suite and pull_request can arrive in any order.
  //
  //     - check_suite(requested, PR_head)
  //
  //       Note: check_suite.pull_requests[] will contain all PRs with this
  //       branch as head.
  //
  //       Note: check_suite.pull_requests[i].head.sha will be the new,
  //       updated PR head sha.
  //
  //     - pull_request(synchronize)
  //
  //     Solution: Ignore all check_suites with non-empty pull_requests[].
  //
  // - New commits are added to PR base branch
  //
  //   Note: In this case pull_request.base.sha does not change, but the merge
  //   commit will be updated to include the new commits to the base branch.
  //
  // - @@ TODO? PR base branch changed (to a different branch)
  //
  //   => pull_request(edited)
  //
  // - PR closed @@ TODO
  //
  //   Also received if base branch is deleted. (And presumably same for head
  //   branch.)
  //
  //   => pull_request(closed)
  //
  //   Cancel CI?
  //
  // - PR merged @@ TODO
  //
  //   => pull_request(merged)
  //
  //   => check_suite(PR_base)
  //
  //   Probably wouldn't want to CI the base again because the PR CI would've
  //   done the equivalent already.
  //
  bool ci_github::
  handle_pull_request (gh_pull_request_event pr, bool warning_success)
  {
    HANDLER_DIAG;

    l3 ([&]{trace << "pull_request event { " << pr << " }";});

    // While we don't need the installation access token in this request,
    // let's obtain it to flush out any permission issues early. Also, it is
    // valid for an hour so we will most likely make use of it.
    //
    optional<string> jwt (generate_jwt (trace, error));
    if (!jwt)
      throw server_error ();

    optional<gh_installation_access_token> iat (
      obtain_installation_access_token (pr.installation.id,
                                        move (*jwt),
                                        error));
    if (!iat)
      throw server_error ();

    l3 ([&]{trace << "installation_access_token { " << *iat << " }";});

    // Note that similar to the branch push case above, while it would have
    // been nice to cancel the previous CI job once the PR head moves (the
    // "synchronize" event), due to the head sharing problem the previous CI
    // job might actually still be relevant (in both local and remote PR
    // cases).

    // @@ TODO Serialize the new service_data fields!
    //

    // Distinguish between local and remote PRs by comparing the head and base
    // repositories' paths.
    //
    service_data::kind_type kind (
      pr.pull_request.head_path == pr.pull_request.base_path
      ? service_data::local
      : service_data::remote);

    // Note: for remote PRs the check_sha will be set later, in
    // build_unloaded_pre_check(), to test merge commit id.
    //
    string check_sha (kind == service_data::local
                      ? pr.pull_request.head_sha
                      : "");

    // Note that PR rebuilds (re-requested) are handled by check_suite().
    //
    service_data sd (warning_success,
                     move (iat->token),
                     iat->expires_at,
                     pr.installation.id,
                     move (pr.repository.node_id),
                     move (pr.repository.clone_url),
                     pr.pull_request.node_id,
                     kind, true /* pre_check */, false /* re_request */,
                     move (check_sha),
                     move (pr.pull_request.head_sha) /* report_sha */,
                     pr.pull_request.number);

    // Create an unloaded CI tenant for the pre-check phase (during which we
    // wait for the PR's merge commit and behindness to become available).
    //
    // Create with an empty service id so that the generated tenant id is used
    // instead during the pre-check phase (so as not to clash with a proper
    // service id for this head commit, potentially created in
    // handle_check_suite() or as another PR).
    //
    tenant_service ts ("", "ci-github", sd.json ());

    // Note: use no delay since we need to start the actual CI (which in turn
    // (re)creates the synthetic conclusion check run) as soon as possible.
    //
    // After this call we will start getting the build_unloaded()
    // notifications -- which will be routed to build_unloaded_pre_check() --
    // until we cancel the tenant or it gets archived after some timeout.
    // (Note that we never actually load this request, we always cancel it;
    // see build_unloaded_pre_check() for details.)
    //
    if (!create (error,
                 warn,
                 verb_ ? &trace : nullptr,
                 *build_db_,
                 move (ts),
                 chrono::seconds (30) /* interval */,
                 chrono::seconds (0) /* delay */))
    {
      fail << "pull request " << pr.pull_request.node_id
           << ": unable to create unloaded pre-check tenant";
    }

    return true;
  }

  function<optional<string> (const tenant_service&)> ci_github::
  build_unloaded (tenant_service&& ts,
                  const diag_epilogue& log_writer) const noexcept
  {
    NOTIFICATION_DIAG (log_writer);

    service_data sd;
    try
    {
      sd = service_data (*ts.data);
    }
    catch (const invalid_argument& e)
    {
      error << "failed to parse service data: " << e;
      return nullptr;
    }

    return sd.pre_check
             ? build_unloaded_pre_check (move (ts), move (sd), log_writer)
             : build_unloaded_load (move (ts), move (sd), log_writer);
  }

  function<optional<string> (const tenant_service&)> ci_github::
  build_unloaded_pre_check (tenant_service&& ts,
                            service_data&& sd,
                            const diag_epilogue& log_writer) const noexcept
  {
    NOTIFICATION_DIAG (log_writer);

    // We get here for PRs only (but both local and remote). The overall
    // plan is as follows:
    //
    // 1. Ask for the mergeability/behind status/test merge commit.
    //
    // 2. If not ready, get called again.
    //
    // 3. If not mergeable, behind, or different head (head changed while
    //    waiting for merge commit and thus differs from what's in the
    //    service_data), cancel the pre-check tenant and do nothing.
    //
    // 4. Otherwise, create an unloaded CI tenant and cancel ourselves. Note
    //    that all re-requested cases are handled elsewhere.
    //
    // Note that in case of a mixed local/remote case, whether we CI the head
    // commit or test merge commit will be racy and there is nothing we can do
    // about (the purely local case can get "upgraded" to mixed after we have
    // started the CI job).
    //

    // Request PR pre-check info (triggering the generation of the test merge
    // commit on the GitHub's side).
    //
    optional<gq_pr_pre_check_info> pc (
      gq_fetch_pull_request_pre_check_info (error,
                                            sd.installation_access.token,
                                            *sd.pr_node_id));

    if (!pc)
    {
      // Test merge commit not available yet: get called again to retry.
      //
      return nullptr;
    }

    // Create the CI tenant if nothing is wrong, otherwise issue diagnostics.
    //
    if (pc->behind)
    {
      l3 ([&]{trace << "ignoring pull request " << *sd.pr_node_id
                    << ": head is behind base";});
    }
    else if (!pc->merge_commit_sha)
    {
      l3 ([&]{trace << "ignoring pull request " << *sd.pr_node_id
                    << ": not auto-mergeable";});
    }
    else if (pc->head_sha != sd.report_sha)
    {
      l3 ([&]{trace << "ignoring pull request " << *sd.pr_node_id
                    << ": head commit has changed";});
    }
    else
    {
      // Create the CI tenant.

      // Update service data's check_sha if this is a remote PR.
      //
      if (sd.kind == service_data::remote)
        sd.check_sha = *pc->merge_commit_sha;

      // Service id that will uniquely identify the CI tenant.
      //
      string sid (sd.repository_node_id + ":" + sd.report_sha);

      // Create an unloaded CI tenant, doing nothing if one already exists
      // (which could've been created by a head branch push or another PR
      // sharing the same head commit).
      //
      // Note: use no delay since we need to (re)create the synthetic
      // conclusion check run as soon as possible.
      //
      // Note that we use the create() API instead of start() since duplicate
      // management is not available in start().
      //
      // After this call we will start getting the build_unloaded()
      // notifications until (1) we load the tenant, (2) we cancel it, or (3)
      // it gets archived after some timeout.
      //
      if (auto pr = create (error, warn, verb_ ? &trace : nullptr,
                            *build_db_,
                            tenant_service (sid, "ci-github", sd.json ()),
                            chrono::seconds (30) /* interval */,
                            chrono::seconds (0) /* delay */,
                            duplicate_tenant_mode::ignore))
      {
        if (pr->second == duplicate_tenant_result::ignored)
        {
          // This PR is sharing a head commit with something else.
          //
          // If this is a local PR then it's probably the branch push, which
          // is expected, so do nothing.
          //
          // If this is a remote PR then it could be anything (branch push,
          // local PR, or another remote PR) which in turn means the CI result
          // may end up being for head, not merge commit. There is nothing we
          // can do about it on our side (the user can enable the head-behind-
          // base protection on their side).
          //
          if (sd.kind == service_data::remote)
          {
            l3 ([&]{trace << "remote pull request " << *sd.pr_node_id
                          << ": CI tenant already exists for " << sid;});
          }
        }
      }
      else
      {
        error << "pull request " << *sd.pr_node_id
              << ": unable to create unloaded CI tenant "
              << "with tenant_service id " << sid;

        // Fall through to cancel.
      }
    }

    // Cancel the pre-check tenant.
    //
    if (!cancel (error, warn, verb_ ? &trace : nullptr,
                 *build_db_, ts.type, ts.id))
    {
      // Should never happen (no such tenant).
      //
      error << "pull request " << *sd.pr_node_id
            << ": failed to cancel pre-check tenant with tenant_service id "
            << ts.id;
    }

    return nullptr;
  }

  function<optional<string> (const tenant_service&)> ci_github::
  build_unloaded_load (tenant_service&& ts,
                       service_data&& sd,
                       const diag_epilogue& log_writer) const noexcept
  {
    NOTIFICATION_DIAG (log_writer);

    // Load the tenant, which is essentially the same for both branch push and
    // PR. The overall plan is as follows:
    //
    // - Create synthetic conclusion check run with the in-progress state. If
    //   unable to, get called again to re-try.
    //
    // - Load the tenant. If unable to, fail the conclusion check run.
    //
    // - Update service data.
    //

    // Get a new installation access token if the current one has expired.
    //
    const gh_installation_access_token* iat (nullptr);
    optional<gh_installation_access_token> new_iat;

    if (system_clock::now () > sd.installation_access.expires_at)
    {
      if (optional<string> jwt = generate_jwt (trace, error))
      {
        new_iat = obtain_installation_access_token (sd.installation_id,
                                                    move (*jwt),
                                                    error);
        if (new_iat)
          iat = &*new_iat;
      }
    }
    else
      iat = &sd.installation_access;

    if (iat == nullptr)
      return nullptr; // Try again on the next call.

    // Create a synthetic check run with an in-progress state. Return the
    // check run on success or nullopt on failure.
    //
    auto create_synthetic_cr = [iat,
                                &sd,
                                &error] (string name) -> optional<check_run>
    {
      check_run cr;
      cr.name = move (name);

      if (gq_create_check_run (error,
                               cr,
                               iat->token,
                               sd.repository_node_id,
                               sd.report_sha,
                               nullopt /* details_url */,
                               build_state::building))
      {
        return cr;
      }
      else
        return nullopt;
    };

    // Update a synthetic check run with success or failure. Return the check
    // run on success or nullopt on failure.
    //
    auto update_synthetic_cr = [iat,
                                &sd,
                                &error] (const string& node_id,
                                         const string& name,
                                         result_status rs,
                                         string summary) -> optional<check_run>
    {
      assert (!node_id.empty ());

      optional<gq_built_result> br (
        gq_built_result (gh_to_conclusion (rs, sd.warning_success),
                         circle (rs) + ' ' + ucase (to_string (rs)),
                         move (summary)));

      check_run cr;
      cr.name = name; // For display purposes only.

      if (gq_update_check_run (error,
                               cr,
                               iat->token,
                               sd.repository_node_id,
                               node_id,
                               nullopt /* details_url */,
                               build_state::built,
                               move (br)))
      {
        assert (cr.state == build_state::built);
        return cr;
      }
      else
        return nullopt;
    };

    // Note that there is a window between receipt of a check_suite or
    // pull_request event and the first bot/worker asking for a task, which
    // could be substantial. We could probably (also) try to (re)create the
    // conclusion checkrun in the webhook handler. @@ Maybe/later.

    // (Re)create the synthetic conclusion check run first in order to convert
    // a potentially completed check suite to building as early as possible.
    //
    string conclusion_node_id; // Conclusion check run node ID.

    if (auto cr = create_synthetic_cr (conclusion_check_run_name))
    {
      l3 ([&]{trace << "created check_run { " << *cr << " }";});

      conclusion_node_id = move (*cr->node_id);
    }

    // Load the CI tenant if the conclusion check run was created.
    //
    if (!conclusion_node_id.empty ())
    {
      string ru; // Repository URL.

      // CI the merge test commit for remote PRs and the head commit for
      // everything else (branch push or local PRs).
      //
      if (sd.kind == service_data::remote)
      {
        // E.g. #pull/28/merge@1b6c9a361086ed93e6f1e67189e82d52de91c49b
        //
        ru = sd.repository_clone_url + "#pull/" + to_string (*sd.pr_number) +
             "/merge@" + sd.check_sha;
      }
      else
        ru = sd.repository_clone_url + '#' + sd.check_sha;

      repository_location rl (move (ru), repository_type::git);

      optional<start_result> r (load (error, warn, verb_ ? &trace : nullptr,
                                      *build_db_,
                                      move (ts),
                                      move (rl)));

      if (!r || r->status != 200)
      {
        if (auto cr = update_synthetic_cr (conclusion_node_id,
                                           conclusion_check_run_name,
                                           result_status::error,
                                           to_check_run_summary (r)))
        {
          l3 ([&]{trace << "updated check_run { " << *cr << " }";});
        }
        else
        {
          // Nothing really we can do in this case since we will not receive
          // any further notifications. Log the error as a last resort.

          error << "failed to load CI tenant " << ts.id
                << " and unable to update conclusion";
        }

        return nullptr; // No need to update service data in this case.
      }
    }
    else if (!new_iat)
      return nullptr; // Nothing to save (but retry on next call).

    return [&error,
            iat = move (new_iat),
            cni = move (conclusion_node_id)]
      (const tenant_service& ts) -> optional<string>
    {
      // NOTE: this lambda may be called repeatedly (e.g., due to
      // transaction being aborted) and so should not move out of its
      // captures.

      service_data sd;
      try
      {
        sd = service_data (*ts.data);
      }
      catch (const invalid_argument& e)
      {
        error << "failed to parse service data: " << e;
        return nullopt;
      }

      if (iat)
        sd.installation_access = *iat;

      if (!cni.empty ())
        sd.conclusion_node_id = cni;

      return sd.json ();
    };
  }

  // Build state change notifications (see tenant-services.hxx for
  // background). Mapping our state transitions to GitHub pose multiple
  // problems:
  //
  // 1. In our model we have the building->queued (interrupted) and
  //    built->queued (rebuild) transitions. We are going to ignore both of
  //    them when notifying GitHub. The first is not important (we expect the
  //    state to go back to building shortly). The second should normally not
  //    happen and would mean that a completed check suite may go back on its
  //    conclusion (which would be pretty confusing for the user). @@@ This
  //    can/will happen on check run rebuild. Distinguish between internal
  //    and external rebuilds?
  //
  //    So, for GitHub notifications, we only have the following linear
  //    transition sequence:
  //
  //    -> queued -> building -> built
  //
  //    Note, however, that because we ignore certain transitions, we can now
  //    observe "degenerate" state changes that we need to ignore:
  //
  //    building -> [queued] -> building
  //    built -> [queued] -> ...
  //
  // 2. As mentioned in tenant-services.hxx, we may observe the notifications
  //    as arriving in the wrong order. Unfortunately, GitHub provides no
  //    mechanisms to help with that. In fact, GitHub does not even prevent
  //    the creation of multiple check runs with the same name (it will always
  //    use the last created instance, regardless of the status, timestamps,
  //    etc). As a result, we cannot, for example, rely on the failure to
  //    create a new check run in response to the queued notification as an
  //    indication of a subsequent notification (e.g., building) having
  //    already occurred.
  //
  //    The only aid in this area that GitHub provides is that it prevents
  //    updating a check run in the built state to a former state (queued or
  //    building). But one can still create a new check run with the same name
  //    and a former state.
  //
  //    (Note that we should also be careful if trying to take advantage of
  //    this "check run override" semantics: each created check run gets a new
  //    URL and while the GitHub UI will always point to the last created when
  //    showing the list of check runs, if the user is already on the previous
  //    check run's URL, nothing will automatically cause them to be
  //    redirected to the new URL. And so the user may sit on the abandoned
  //    check run waiting forever for it to be completed.)
  //
  //    As a result, we will deal with the out of order problem differently
  //    depending on the notification:
  //
  //    queued    Skip if there is already a check run in service data,
  //              otherwise create new.
  //
  //    building  Skip if there is no check run in service data or it's
  //              not in the queued state, otherwise update.
  //
  //    built     Update if there is check run in service data unless its
  //              state is built, otherwise create new.
  //
  //    The rationale for this semantics is as follows: the building
  //    notification is a "nice to have" and can be skipped if things are not
  //    going normally. In contrast, the built notification cannot be skipped
  //    and we must either update the existing check run or create a new one
  //    (hopefully overriding the one created previously, if any). Note that
  //    the likelihood of the built notification being performed at the same
  //    time as queued/building is quite low (unlike queued and building).
  //
  //    Note also that with this semantics it's unlikely but possible that we
  //    attempt to update the service data in the wrong order. Specifically, it
  //    feels like this should not be possible in the ->building transition
  //    since we skip the building notification unless the check run in the
  //    service data is already in the queued state. But it is theoretically
  //    possible in the ->built transition. For example, we may be updating
  //    the service data for the queued notification after it has already been
  //    updated by the built notification. In such cases we should not be
  //    overriding the latter state (built) with the former (queued).
  //
  // 3. We may not be able to "conclusively" notify GitHub, for example, due
  //    to a transient network error. The "conclusively" part means that the
  //    notification may or may not have gone through (though it feels the
  //    common case will be the inability to send the request rather than
  //    receive the reply).
  //
  //    In such cases, we record in the service data that the notification was
  //    not synchronized and in subsequent notifications we do the best we can:
  //    if we have node_id, then we update, otherwise, we create (potentially
  //    overriding the check run created previously).
  //
  function<optional<string> (const tenant_service&)> ci_github::
  build_queued (const tenant_service& ts,
                const vector<build>& builds,
                optional<build_state> istate,
                const build_queued_hints& hs,
                const diag_epilogue& log_writer) const noexcept
  {
    NOTIFICATION_DIAG (log_writer);

    service_data sd;
    try
    {
      sd = service_data (*ts.data);
    }
    catch (const invalid_argument& e)
    {
      error << "failed to parse service data: " << e;
      return nullptr;
    }

    // The builds for which we will be creating check runs.
    //
    vector<reference_wrapper<const build>> bs;
    vector<check_run> crs; // Parallel to bs.

    // Exclude the builds for which we won't be creating check runs.
    //
    for (const build& b: builds)
    {
      string bid (gh_check_run_name (b)); // Full build id.

      if (const check_run* scr = sd.find_check_run (bid))
      {
        // Another notification has already stored this check run.
        //
        if (!istate)
        {
          // Out of order queued notification.
          //
          warn << "check run " << bid << ": out of order queued "
               << "notification; existing state: " << scr->state_string ();
        }
        else if (*istate == build_state::built)
        {
          // Unexpected built->queued transition (rebuild).
          //
          warn << "check run " << bid << ": unexpected rebuild";
        }
        else
        {
          // Ignore interrupted.
          //
          assert (*istate == build_state::building);
        }
      }
      else
      {
        // No stored check run for this build so prepare to create one.
        //
        bs.push_back (b);

        crs.emplace_back (move (bid),
                          gh_check_run_name (b, &hs),
                          nullopt, /* node_id */
                          build_state::queued,
                          false /* state_synced */);
      }
    }

    if (bs.empty ()) // Nothing to do.
      return nullptr;

    // Get a new installation access token if the current one has expired.
    //
    const gh_installation_access_token* iat (nullptr);
    optional<gh_installation_access_token> new_iat;

    if (system_clock::now () > sd.installation_access.expires_at)
    {
      if (optional<string> jwt = generate_jwt (trace, error))
      {
        new_iat = obtain_installation_access_token (sd.installation_id,
                                                    move (*jwt),
                                                    error);
        if (new_iat)
          iat = &*new_iat;
      }
    }
    else
      iat = &sd.installation_access;

    // Note: we treat the failure to obtain the installation access token the
    // same as the failure to notify GitHub (state is updated by not marked
    // synced).
    //
    if (iat != nullptr)
    {
      // Create a check_run for each build as a single request.
      //
      if (gq_create_check_runs (error,
                                crs,
                                iat->token,
                                sd.repository_node_id, sd.report_sha,
                                build_state::queued))
      {
        for (const check_run& cr: crs)
        {
          // We can only create a check run in the queued state.
          //
          assert (cr.state == build_state::queued);
          l3 ([&]{trace << "created check_run { " << cr << " }";});
        }
      }
    }

    return [bs = move (bs),
            iat = move (new_iat),
            crs = move (crs),
            error = move (error),
            warn = move (warn)] (const tenant_service& ts) -> optional<string>
    {
      // NOTE: this lambda may be called repeatedly (e.g., due to transaction
      // being aborted) and so should not move out of its captures.

      service_data sd;
      try
      {
        sd = service_data (*ts.data);
      }
      catch (const invalid_argument& e)
      {
        error << "failed to parse service data: " << e;
        return nullopt;
      }

      if (iat)
        sd.installation_access = *iat;

      for (size_t i (0); i != bs.size (); ++i)
      {
        const check_run& cr (crs[i]);

        // Note that this service data may not be the same as what we observed
        // in the build_queued() function above. For example, some check runs
        // that we have queued may have already transitioned to built. So we
        // skip any check runs that are already present.
        //
        if (const check_run* scr = sd.find_check_run (cr.build_id))
        {
          // Doesn't looks like printing new/existing check run node_id will
          // be of any help.
          //
          warn << "check run " << cr.build_id << ": out of order queued "
               << "notification service data update; existing state: "
               << scr->state_string ();
        }
        else
          sd.check_runs.push_back (cr);
      }

      return sd.json ();
    };
  }

  function<optional<string> (const tenant_service&)> ci_github::
  build_building (const tenant_service& ts,
                  const build& b,
                  const diag_epilogue& log_writer) const noexcept
  {
    NOTIFICATION_DIAG (log_writer);

    service_data sd;
    try
    {
      sd = service_data (*ts.data);
    }
    catch (const invalid_argument& e)
    {
      error << "failed to parse service data: " << e;
      return nullptr;
    }

    optional<check_run> cr; // Updated check run.
    string bid (gh_check_run_name (b)); // Full build id.

    if (check_run* scr = sd.find_check_run (bid)) // Stored check run.
    {
      // Update the check run if it exists on GitHub and the queued
      // notification updated the service data, otherwise do nothing.
      //
      if (scr->state == build_state::queued)
      {
        if (scr->node_id)
        {
          cr = move (*scr);
          cr->state_synced = false;
        }
        else
        {
          // Network error during queued notification, ignore.
        }
      }
      else
        warn << "check run " << bid << ": out of order building "
             << "notification; existing state: " << scr->state_string ();
    }
    else
      warn << "check run " << bid << ": out of order building "
           << "notification; no check run state in service data";

    if (!cr)
      return nullptr;

    // Get a new installation access token if the current one has expired.
    //
    const gh_installation_access_token* iat (nullptr);
    optional<gh_installation_access_token> new_iat;

    if (system_clock::now () > sd.installation_access.expires_at)
    {
      if (optional<string> jwt = generate_jwt (trace, error))
      {
        new_iat = obtain_installation_access_token (sd.installation_id,
                                                    move (*jwt),
                                                    error);
        if (new_iat)
          iat = &*new_iat;
      }
    }
    else
      iat = &sd.installation_access;

    // Note: we treat the failure to obtain the installation access token the
    // same as the failure to notify GitHub (state is updated but not marked
    // synced).
    //
    if (iat != nullptr)
    {
      if (gq_update_check_run (error,
                               *cr,
                               iat->token,
                               sd.repository_node_id,
                               *cr->node_id,
                               details_url (b),
                               build_state::building))
      {
        // Do nothing further if the state was already built on GitHub (note
        // that this is based on the above-mentioned special GitHub semantics
        // of preventing changes to the built status).
        //
        if (cr->state == build_state::built)
        {
          warn << "check run " << bid << ": already in built state on GitHub";
          return nullptr;
        }

        assert (cr->state == build_state::building);
        l3 ([&]{trace << "updated check_run { " << *cr << " }";});
      }
    }

    return [iat = move (new_iat),
            cr = move (*cr),
            error = move (error),
            warn = move (warn)] (const tenant_service& ts) -> optional<string>
    {
      // NOTE: this lambda may be called repeatedly (e.g., due to transaction
      // being aborted) and so should not move out of its captures.

      service_data sd;
      try
      {
        sd = service_data (*ts.data);
      }
      catch (const invalid_argument& e)
      {
        error << "failed to parse service data: " << e;
        return nullopt;
      }

      if (iat)
        sd.installation_access = *iat;

      // Update the check run only if it is in the queued state.
      //
      if (check_run* scr = sd.find_check_run (cr.build_id))
      {
        if (scr->state == build_state::queued)
          *scr = cr;
        else
        {
          warn << "check run " << cr.build_id << ": out of order building "
               << "notification service data update; existing state: "
               << scr->state_string ();
        }
      }
      else
        warn << "check run " << cr.build_id << ": service data state has "
             << "disappeared";

      return sd.json ();
    };
  }

  function<optional<string> (const tenant_service&)> ci_github::
  build_built (const tenant_service& ts,
               const build& b,
               const diag_epilogue& log_writer) const noexcept
  {
    // @@ TODO Include service_data::event_node_id and perhaps ts.id in
    //    diagnostics? E.g. when failing to update check runs we print the
    //    build ID only.
    //
    NOTIFICATION_DIAG (log_writer);

    service_data sd;
    try
    {
      sd = service_data (*ts.data);
    }
    catch (const invalid_argument& e)
    {
      error << "failed to parse service data: " << e;
      return nullptr;
    }

    // Here we need to update the state of this check run and, if there are no
    // more unbuilt ones, update the synthetic conclusion check run.
    //
    // Absent means we still have unbuilt check runs.
    //
    optional<result_status> conclusion (*b.status);

    check_run cr; // Updated check run.
    {
      string bid (gh_check_run_name (b)); // Full build id.

      optional<check_run> scr;
      for (check_run& cr: sd.check_runs)
      {
        if (cr.build_id == bid)
        {
          assert (!scr);
          scr = move (cr);
        }
        else
        {
          if (cr.state == build_state::built)
          {
            assert (cr.status);

            if (conclusion)
              *conclusion |= *cr.status;
          }
          else
            conclusion = nullopt;
        }

        if (scr && !conclusion.has_value ())
          break;
      }

      if (scr)
      {
        if (scr->state != build_state::building)
        {
          warn << "check run " << bid << ": out of order built notification; "
               << "existing state: " << scr->state_string ();
        }

        // Do nothing if already built (e.g., rebuild).
        //
        if (scr->state == build_state::built)
          return nullptr;

        cr = move (*scr);
      }
      else
      {
        warn << "check run " << bid << ": out of order built notification; "
             << "no check run state in service data";

        // Note that we have no hints here and so have to use the full build
        // id for name.
        //
        cr.build_id = move (bid);
        cr.name = cr.build_id;
      }

      cr.state_synced = false;
    }

    // Get a new installation access token if the current one has expired.
    //
    const gh_installation_access_token* iat (nullptr);
    optional<gh_installation_access_token> new_iat;

    if (system_clock::now () > sd.installation_access.expires_at)
    {
      if (optional<string> jwt = generate_jwt (trace, error))
      {
        new_iat = obtain_installation_access_token (sd.installation_id,
                                                    move (*jwt),
                                                    error);
        if (new_iat)
          iat = &*new_iat;
      }
    }
    else
      iat = &sd.installation_access;

    // Note: we treat the failure to obtain the installation access token the
    // same as the failure to notify GitHub (state is updated but not marked
    // synced).
    //
    if (iat != nullptr)
    {
      // Prepare the check run's summary field (the build information in an
      // XHTML table).
      //
      string sm; // Summary.
      {
        using namespace web::xhtml;

        ostringstream os;
        xml::serializer s (os, "check_run_summary");

        // This hack is required to disable XML element name prefixes (which
        // GitHub does not like). Note that this adds an xmlns declaration for
        // the XHTML namespace which for now GitHub appears to ignore. If that
        // ever becomes a problem, then we should redo this with raw XML
        // serializer calls.
        //
        struct table: element
        {
          table (): element ("table") {}

          void
          start (xml::serializer& s) const override
          {
            s.start_element (xmlns, name);
            s.namespace_decl (xmlns, "");
          }
        } TABLE;

        // Serialize a result row (colored circle, result text, log URL) for
        // an operation and result_status.
        //
        auto tr_result = [this, &b] (xml::serializer& s,
                                     const string& op,
                                     result_status rs)
        {
          // The log URL.
          //
          string lu (build_log_url (options_->host (),
                                    options_->root (),
                                    b,
                                    op != "result" ? &op : nullptr));

          s << TR
            <<   TD << EM << op << ~EM << ~TD
            <<   TD
            <<     circle (rs) << ' '
            <<     CODE << to_string (rs) << ~CODE
            <<     " (" << A << HREF << lu << ~HREF << "log" << ~A << ')'
            <<   ~TD
            << ~TR;
        };

        // Serialize the summary to an XHTML table.
        //
        s << TABLE
          <<   TBODY;

        tr_result (s, "result", *b.status);

        s <<     TR
          <<       TD << EM   << "package"      << ~EM   << ~TD
          <<       TD << CODE << b.package_name << ~CODE << ~TD
          <<     ~TR
          <<     TR
          <<       TD << EM   << "version"         << ~EM   << ~TD
          <<       TD << CODE << b.package_version << ~CODE << ~TD
          <<     ~TR
          <<     TR
          <<       TD << EM << "toolchain" << ~EM << ~TD
          <<       TD
          <<         CODE
          <<           b.toolchain_name << '-' << b.toolchain_version.string ()
          <<         ~CODE
          <<       ~TD
          <<     ~TR
          <<     TR
          <<       TD << EM   << "target"           << ~EM   << ~TD
          <<       TD << CODE << b.target.string () << ~CODE << ~TD
          <<     ~TR
          <<     TR
          <<       TD << EM   << "target config"      << ~EM   << ~TD
          <<       TD << CODE << b.target_config_name << ~CODE << ~TD
          <<     ~TR
          <<     TR
          <<       TD << EM   << "package config"      << ~EM   << ~TD
          <<       TD << CODE << b.package_config_name << ~CODE << ~TD
          <<     ~TR;

        for (const operation_result& r: b.results)
          tr_result (s, r.operation, r.status);

        s <<   ~TBODY
          << ~TABLE;

        sm = os.str ();
      }

      gq_built_result br (
        gh_to_conclusion (*b.status, sd.warning_success),
        circle (*b.status) + ' ' + ucase (to_string (*b.status)),
        move (sm));

      if (cr.node_id)
      {
        // Update existing check run to built.
        //
        if (gq_update_check_run (error,
                                 cr,
                                 iat->token,
                                 sd.repository_node_id,
                                 *cr.node_id,
                                 details_url (b),
                                 build_state::built,
                                 move (br)))
        {
          assert (cr.state == build_state::built);
          l3 ([&]{trace << "updated check_run { " << cr << " }";});
        }
      }
      else
      {
        // Create new check run.
        //
        // Note that we don't have build hints so will be creating this check
        // run with the full build id as name. In the unlikely event that an
        // out of order build_queued() were to run before we've saved this
        // check run to the service data it will create another check run with
        // the shortened name which will never get to the built state.
        //
        if (gq_create_check_run (error,
                                 cr,
                                 iat->token,
                                 sd.repository_node_id,
                                 sd.report_sha,
                                 details_url (b),
                                 build_state::built,
                                 move (br)))
        {
          assert (cr.state == build_state::built);
          l3 ([&]{trace << "created check_run { " << cr << " }";});
        }
      }

      if (cr.state_synced)
      {
        // Check run was created/updated successfully to built.
        //
        // @@ TMP Feels like this should also be done inside
        //    gq_{create,update}_check_run() -- where cr.state is set if the
        //    create/update succeeds -- but I think we didn't want to pass a
        //    result_status into a gq_ function because converting to a GitHub
        //    conclusion/title/summary is reasonably complicated.
        //
        //    @@@ We need to redo that code:
        //
        //    - Pass the vector of check runs with new state (and status) set.
        //    - Update synchronized flag inside those functions.
        //    - Update the state to built if it's already built on GitHub --
        //      but then what do we set the status to?
        //
        //      @@ TMP This scenario can only arise for updates to building.
        //         For creations a new CR will always be created so the
        //         returned state will always be what we asked for; and we
        //         never update to queued.
        //
        //         As for updates to building, if GH has already been updated
        //         to built then the build_built() lambda will soon save the
        //         built state and valid status so nothing more should need to
        //         be done. In fact, whenever updating to building we do stop
        //         immediately if it's already built on GH.
        //
        //    - Maybe signal in return value (optional<bool>?) that there is
        //      a discrepancy.
        //
        cr.status = b.status;

        // Update the conclusion check run if all check runs are now built.
        //
        if (conclusion)
        {
          assert (sd.conclusion_node_id);

          result_status rs (*conclusion);

          optional<gq_built_result> br (
            gq_built_result (gh_to_conclusion (rs, sd.warning_success),
                             circle (rs) + ' ' + ucase (to_string (rs)),
                             "All configurations are built"));

          check_run cr;

          // Set some fields for display purposes.
          //
          cr.node_id = *sd.conclusion_node_id;
          cr.name = conclusion_check_run_name;

          if (gq_update_check_run (error,
                                   cr,
                                   iat->token,
                                   sd.repository_node_id,
                                   *sd.conclusion_node_id,
                                   nullopt /* details_url */,
                                   build_state::built,
                                   move (br)))
          {
            assert (cr.state == build_state::built);
            l3 ([&]{trace << "updated conclusion check_run { " << cr << " }";});
          }
          else
          {
            // Nothing we can do here except log the error.
            //
            error << "tenant_service id " << ts.id
                  << ": unable to update conclusion check run "
                  << *sd.conclusion_node_id;
          }
        }
      }
    }

    return [iat = move (new_iat),
            cr = move (cr),
            error = move (error),
            warn = move (warn)] (const tenant_service& ts) -> optional<string>
    {
      // NOTE: this lambda may be called repeatedly (e.g., due to transaction
      // being aborted) and so should not move out of its captures.

      service_data sd;
      try
      {
        sd = service_data (*ts.data);
      }
      catch (const invalid_argument& e)
      {
        error << "failed to parse service data: " << e;
        return nullopt;
      }

      if (iat)
        sd.installation_access = *iat;

      if (check_run* scr = sd.find_check_run (cr.build_id))
      {
        // This will most commonly generate a duplicate warning (see above).
        // We could save the old state and only warn if it differs but let's
        // not complicate things for now.
        //
#if 0
        if (scr->state != build_state::building)
        {
          warn << "check run " << cr.build_id << ": out of order built "
               << "notification service data update; existing state: "
               << scr->state_string ();
        }
#endif
        *scr = cr;
      }
      else
        sd.check_runs.push_back (cr);

      return sd.json ();
    };
  }

  string ci_github::
  details_url (const build& b) const
  {
    return options_->host ()                                          +
      "/@" + b.tenant                                                 +
      "?builds=" + mime_url_encode (b.package_name.string ())         +
      "&pv=" + b.package_version.string ()                            +
      "&tg=" + mime_url_encode (b.target.string ())                   +
      "&tc=" + mime_url_encode (b.target_config_name)                 +
      "&pc=" + mime_url_encode (b.package_config_name)                +
      "&th=" + mime_url_encode (b.toolchain_version.string ());
  }

  optional<string> ci_github::
  generate_jwt (const basic_mark& trace,
                const basic_mark& error) const
  {
    string jwt;
    try
    {
      // Set token's "issued at" time 60 seconds in the past to combat clock
      // drift (as recommended by GitHub).
      //
      jwt = brep::generate_jwt (
          *options_,
          options_->ci_github_app_private_key (),
          to_string (options_->ci_github_app_id ()),
          chrono::seconds (options_->ci_github_jwt_validity_period ()),
          chrono::seconds (60));

      l3 ([&]{trace << "JWT: " << jwt;});
    }
    catch (const system_error& e)
    {
      error << "unable to generate JWT (errno=" << e.code () << "): " << e;
      return nullopt;
    }

    return jwt;
  }

  // There are three types of GitHub API authentication:
  //
  //   1) Authenticating as an app. Used to access parts of the API concerning
  //      the app itself such as getting the list of installations. (Need to
  //      authenticate as an app as part of authenticating as an app
  //      installation.)
  //
  //   2) Authenticating as an app installation (on a user or organisation
  //      account). Used to access resources belonging to the user/repository
  //      or organisation the app is installed in.
  //
  //   3) Authenticating as a user. Used to perform actions as the user.
  //
  // We need to authenticate as an app installation (2).
  //
  // How to authenticate as an app installation
  //
  // Reference:
  // https://docs.github.com/en/apps/creating-github-apps/authenticating-with-a-github-app/authenticating-as-a-github-app-installation
  //
  // The final authentication token we need is an installation access token
  // (IAT), valid for one hour, which we will pass in the `Authentication`
  // header of our Github API requests:
  //
  //   Authorization: Bearer <INSTALLATION_ACCESS_TOKEN>
  //
  // To generate an IAT:
  //
  // - Generate a JSON Web Token (JWT)
  //
  // - Get the installation ID. This will be included in the webhook request
  //   in our case
  //
  // - Send a POST to /app/installations/<INSTALLATION_ID>/access_tokens which
  //   includes the JWT (`Authorization: Bearer <JWT>`). The response will
  //   include the IAT. Can pass the name of the repository included in the
  //   webhook request to restrict access, otherwise we get access to all
  //   repos covered by the installation if installed on an organisation for
  //   example.
  //
  optional<gh_installation_access_token> ci_github::
  obtain_installation_access_token (uint64_t iid,
                                    string jwt,
                                    const basic_mark& error) const
  {
    gh_installation_access_token iat;
    try
    {
      // API endpoint.
      //
      string ep ("app/installations/" + to_string (iid) + "/access_tokens");

      uint16_t sc (
          github_post (iat, ep, strings {"Authorization: Bearer " + jwt}));

      // Possible response status codes from the access_tokens endpoint:
      //
      // 201 Created
      // 401 Requires authentication
      // 403 Forbidden
      // 404 Resource not found
      // 422 Validation failed, or the endpoint has been spammed.
      //
      // Note that the payloads of non-201 status codes are undocumented.
      //
      if (sc != 201)
      {
        error << "unable to get installation access token: error HTTP "
              << "response status " << sc;
        return nullopt;
      }

      // Create a clock drift safety window.
      //
      iat.expires_at -= chrono::minutes (5);
    }
    catch (const json::invalid_json_input& e)
    {
      // Note: e.name is the GitHub API endpoint.
      //
      error << "malformed JSON in response from " << e.name << ", line: "
            << e.line << ", column: " << e.column << ", byte offset: "
            << e.position << ", error: " << e;
      return nullopt;
    }
    catch (const invalid_argument& e)
    {
      error << "malformed header(s) in response: " << e;
      return nullopt;
    }
    catch (const system_error& e)
    {
      error << "unable to get installation access token (errno=" << e.code ()
            << "): " << e.what ();
      return nullopt;
    }

    return iat;
  }
}
