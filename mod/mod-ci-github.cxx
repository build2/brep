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

#include <cerrno>
#include <cstdlib>   // strtoull()
#include <stdexcept>

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
    HANDLER_DIAG;

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
    if (options_->ci_github_app_webhook_secret_specified ())
    {
      if (!options_->build_config_specified ())
        fail << "package building functionality must be enabled";

      if (!options_->ci_github_app_id_private_key_specified ())
        fail << "no app id/private key mappings configured";

      for (const auto& pr: options_->ci_github_app_id_private_key ())
      {
        if (pr.second.relative ())
          fail << "ci-github-app-id-private-key path must be absolute";
      }

      // Read the webhook secret from the configured path.
      //
      {
        const path& p (options_->ci_github_app_webhook_secret ());

        if (p.relative ())
          fail << "ci-github-app-webhook-secret path must be absolute";

        try
        {
          ifdstream is (p);
          getline (is, webhook_secret_, '\0');

          // Trim leading/trailing whitespaces (presumably GitHub does the
          // same in its web UI).
          //
          if (trim (webhook_secret_).empty ())
            fail << "empty webhook secret in " << p;
        }
        catch (const io_error& e)
        {
          fail << "unable to read webhook secret from " << p << ": " << e;
        }
      }

      if (!options_->ci_github_app_id_name_specified ())
        fail << "no app id/app name mappings configured";

      for (const auto& pr: options_->ci_github_app_id_name ())
      {
        if (pr.second.empty ())
        {
          fail << "ci-github-app-id-name value is empty for app id "
               << pr.first;
        }
      }

      ci_start::init (make_shared<options::ci_start> (*options_));

      database_module::init (*options_, options_->build_db_retry ());
    }
  }

  bool ci_github::
  handle (request& rq, response& rs)
  {
    using namespace bpkg;

    HANDLER_DIAG;

    if (build_db_ == nullptr)
      throw invalid_request (501, "GitHub CI submission not implemented");

    // The request's query parameters.
    //
    const name_values& rps (rq.parameters (1024, true /* url_only */));

    // Process the handler's default parameter (named "_").
    //
    // Note that the default parameter currently only gets used for forced
    // rebuild requests (see handle_forced_check_suite_rebuild() in the
    // header). All of the GitHub webhook requests are handled separately
    // below.
    //
    // Also note that the default parameter gets renamed from "ci-github" to
    // "_" in request_proxy::parameters() and that it will have been removed
    // if it had no value at all (not even an empty one).
    //
    if (!rps.empty () && rps.front ().name == "_")
    {
      const optional<string>& dpv (rps.front ().value); // Default param value.
      assert (dpv); // Should have been removed from rps if no value.

      if (*dpv == "rerequest") // Forced rebuild.
        return handle_forced_check_suite_rebuild (rps, rs);
      else if (!dpv->empty ())
        throw invalid_request (400, "invalid default parameter value '" +
                                    *dpv + '\'');
    }

    // Handle GitHub webhook requests.

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
          if (h.value->compare (0, 7, "sha256=") != 0)
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
      string h (compute_hmac (*options_,
                              body.data (),
                              body.size (),
                              webhook_secret_.c_str ()));

      if (icasecmp (h, hmac) != 0)
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

    // Process the `app-id` and `warning` webhook request query parameters.
    //
    uint64_t app_id;
    bool warning_success;
    {
      bool ai (false), wa (false);

      auto badreq = [] (const string& m)
      {
        throw invalid_request (400, m);
      };

      for (const name_value& rp: rps)
      {
        if (rp.name == "app-id")
        {
          if (!rp.value)
            badreq ("missing 'app-id' webhook query parameter value");

          ai = true;

          // Parse the app id value.
          //
          const char* b (rp.value->c_str ());
          char* e (nullptr);
          errno = 0; // We must clear it according to POSIX.
          app_id = strtoull (b, &e, 10);
          if (errno == ERANGE || e == b || *e != '\0')
          {
            badreq ("invalid 'app-id' webhook query parameter value: '" +
                    *rp.value + '\'');
          }
        }
        else if (rp.name == "warning")
        {
          if (!rp.value)
            badreq ("missing 'warning' webhook query parameter value");

          wa = true;
          const string& v (*rp.value);

          if      (v == "success") warning_success = true;
          else if (v == "failure") warning_success = false;
          else
            badreq ("invalid 'warning' webhook query parameter value: '" + v +
                    '\'');
        }
      }

      if (!ai) badreq ("missing 'app-id' webhook query parameter");
      if (!wa) badreq ("missing 'warning' webhook query parameter");
    }

    // There is a webhook event (specified in the x-github-event header) and
    // each event contains a bunch of actions (specified in the JSON request
    // body).
    //
    // Note: "GitHub continues to add new event types and new actions to
    // existing event types." As a result we ignore known actions that we are
    // not interested in and log and ignore unknown actions. The thinking here
    // is that we want to be "notified" of new actions at which point we can
    // decide whether to ignore them or to handle.
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

      if (cs.check_suite.app_id != app_id)
      {
        fail << "webhook check_suite app.id " << cs.check_suite.app_id
             << " does not match app-id query parameter " << app_id;
      }

      if (cs.action == "requested")
      {
        // Branch pushes are handled in handle_branch_push() so ignore this
        // event.
        //
        return true;
      }
      else if (cs.action == "rerequested")
      {
        // Someone manually requested to re-run all the check runs in this
        // check suite. Treat as a new request.
        //
        return handle_check_suite_rerequest (move (cs), warning_success);
      }
      else if (cs.action == "completed")
      {
        // GitHub thinks that "all the check runs in this check suite have
        // completed and a conclusion is available". Check with our own
        // bookkeeping and log an error if there is a mismatch.
        //
        return handle_check_suite_completed (move (cs), warning_success);
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
    else if (event == "check_run")
    {
      gh_check_run_event cr;
      try
      {
        json::parser p (body.data (), body.size (), "check_run event");

        cr = gh_check_run_event (p);
      }
      catch (const json::invalid_json_input& e)
      {
        string m ("malformed JSON in " + e.name + " request body");

        error << m << ", line: " << e.line << ", column: " << e.column
              << ", byte offset: " << e.position << ", error: " << e;

        throw invalid_request (400, move (m));
      }

      if (cr.check_run.app_id != app_id)
      {
        fail << "webhook check_run app.id " << cr.check_run.app_id
             << " does not match app-id query parameter " << app_id;
      }

      if (cr.action == "rerequested")
      {
        // Someone manually requested to re-run a specific check run.
        //
        return handle_check_run_rerequest (move (cr), warning_success);
      }
#if 0
      // It looks like we shouldn't be receiving these since we are not
      // subscribed to them.
      //
      else if (cr.action == "created"   ||
               cr.action == "completed" ||
               cr.action == "requested_action")
      {
      }
#endif
      else
      {
        // Ignore unknown actions by sending a 200 response with empty body
        // but also log as an error since we want to notice new actions.
        //
        error << "unknown action '" << cr.action << "' in check_run event";

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

      // Store the app-id webhook query parameter in the gh_pull_request_event
      // object (see gh_pull_request for an explanation).
      //
      // When we receive the other webhooks we do check that the app ids in
      // the payload and query match but here we have to assume it is valid.
      //
      pr.pull_request.app_id = app_id;

      if (pr.action == "opened" ||
          pr.action == "synchronize")
      {
        // opened
        //   A pull request was opened.
        //
        // synchronize
        //   A pull request's head branch was updated from the base branch or
        //   new commits were pushed to the head branch. (Note that there is
        //   no equivalent event for the base branch.)
        //
        // Note that both cases are handled similarly: we start a new CI
        // request which will be reported on the new commit id.
        //
        return handle_pull_request (move (pr), warning_success);
      }
      else if (pr.action == "edited")
      {
        // PR base branch changed (to a different branch) besides other
        // irrelevant changes (title, body, etc).
        //
        // This is in a sense a special case of the base branch moving. In
        // that case we don't do anything (due to the head sharing problem)
        // relying instead on the branch protection rule. So it makes sense
        // to do the same here.
        //
        return true;
      }
      else if (pr.action == "closed")
      {
        // PR has been closed (as merged or not; see merged member). Also
        // apparently received if base branch is deleted (and the same
        // for head branch). See also the reopened event below.
        //
        // While it may seem natural to cancel the CI for the closed PR, it
        // might actually be useful to have a completed CI record. GitHub
        // doesn't prevent us from publishing CI results for the closed PR
        // (even if both base and head branches were deleted). And if such a
        // PR is reopened, the CI results remain.
        //
        return true;
      }
      else if (pr.action == "reopened")
      {
        // Previously closed PR has been reopened.
        //
        // Since we don't cancel the CI for a closed PR, there is nothing
        // to do if it is reopened.
        //
        return true;
      }
      else if (pr.action == "assigned"               ||
               pr.action == "auto_merge_disabled"    ||
               pr.action == "auto_merge_enabled"     ||
               pr.action == "converted_to_draft"     ||
               pr.action == "demilestoned"           ||
               pr.action == "dequeued"               ||
               pr.action == "enqueued"               ||
               pr.action == "labeled"                ||
               pr.action == "locked"                 ||
               pr.action == "milestoned"             ||
               pr.action == "ready_for_review"       ||
               pr.action == "review_request_removed" ||
               pr.action == "review_requested"       ||
               pr.action == "unassigned"             ||
               pr.action == "unlabeled"              ||
               pr.action == "unlocked")
      {
        // These have no relation to CI.
        //
        return true;
      }
      else
      {
        // Ignore unknown actions by sending a 200 response with empty body
        // but also log as an error since we want to notice new actions.
        //
        error << "unknown action '" << pr.action << "' in pull_request event";

        return true;
      }
    }
    else if (event == "push")
    {
      // Push events are triggered by branch pushes, branch creation, and
      // branch deletion.
      //
      gh_push_event ps;
      try
      {
        json::parser p (body.data (), body.size (), "push event");

        ps = gh_push_event (p);
      }
      catch (const json::invalid_json_input& e)
      {
        string m ("malformed JSON in " + e.name + " request body");

        error << m << ", line: " << e.line << ", column: " << e.column
              << ", byte offset: " << e.position << ", error: " << e;

        throw invalid_request (400, move (m));
      }

      // Store the app-id webhook query parameter in the gh_push_event
      // object (see gh_push_event for an explanation).
      //
      // When we receive the other webhooks we do check that the app ids in
      // the payload and query match but here we have to assume it is valid.
      //
      ps.app_id = app_id;

      // Note that the push request event has no action.
      //
      return handle_branch_push (move (ps), warning_success);
    }
    // Ignore marketplace_purchase events (sent by the GitHub Marketplace) by
    // sending a 200 response with empty body. We offer a free plan only and
    // do not support user accounts so there is nothing to be done.
    //
    else if (event == "marketplace_purchase")
    {
      return true;
    }
    // Ignore GitHub App installation events by sending a 200 response with
    // empty body. These are triggered when a user installs a GitHub App in a
    // repository or organization.
    //
    else if (event == "installation")
    {
      return true;
    }
    // Ignore ping events by sending a 200 response with empty body. This
    // event occurs when you create a new webhook. The ping event is a
    // confirmation from GitHub that you configured the webhook correctly. One
    // of its triggers is listing an App on the GitHub Marketplace.
    //
    else if (event == "ping")
    {
      return true;
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
  static const string conclusion_check_run_basename ("CONCLUSION");

  // Yellow circle.
  //
  static const string conclusion_building_title ("\U0001F7E1 IN PROGRESS");
  static const string conclusion_building_summary (
    "Waiting for all the builds to complete.");

  // "Medium white" circle.
  //
  static const string check_run_queued_title ("\U000026AA QUEUED");
  static const string check_run_queued_summary (
    "Waiting for the build to start.");

  // Yellow circle.
  //
  static const string check_run_building_title ("\U0001F7E1 BUILDING");
  static const string check_run_building_summary (
    "Waiting for the build to complete.");

  // Return the colored circle corresponding to a result_status.
  //
  // Note: the rest of the title is produced by to_string(result_status).
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

  bool ci_github::
  handle_branch_push (gh_push_event ps, bool warning_success)
  {
    HANDLER_DIAG;

    l3 ([&]{trace << "push event { " << ps << " }";});

    // Cancel the CI tenant associated with the overwritten/deleted previous
    // head commit if this is a forced push or a branch deletion.
    //
    if (ps.forced || ps.deleted)
    {
      // Service id that will uniquely identify the CI tenant.
      //
      string sid (ps.repository.node_id + ':' + ps.before);

      // Note that it's possible this commit still exists in another branch so
      // we do refcount-aware cancel.
      //
      if (optional<tenant_service> ts = cancel (error, warn,
                                                verb_ ? &trace : nullptr,
                                                *build_db_, retry_max_,
                                                "ci-github", sid,
                                                true /* ref_count */))
      {
        l3 ([&]{trace << (ps.forced ? "forced push " + ps.after + " to "
                                    : "deletion of ")
                      << ps.ref << ": attempted to cancel CI of previous"
                      << " head commit with tenant_service id " << sid
                      << " (ref_count: " << ts->ref_count << ')';});
      }
      else
      {
        // It's possible that there was no CI for the previous commit for
        // various reasons (e.g., CI was not enabled).
        //
        l3 ([&]{trace << (ps.forced ? "forced push " + ps.after + " to "
                                    : "deletion of ")
                      << ps.ref << ": failed to cancel CI of previous"
                      << " head commit with tenant_service id " << sid;});
      }
    }

    if (ps.deleted)
      return true; // Do nothing further if this was a branch deletion.

    // While we don't need the installation access token in this request,
    // let's obtain it to flush out any permission issues early. Also, it is
    // valid for an hour so we will most likely make use of it.
    //
    optional<string> jwt (generate_jwt (ps.app_id, trace, error));
    if (!jwt)
      throw server_error ();

    optional<gh_installation_access_token> iat (
        obtain_installation_access_token (ps.installation.id,
                                          move (*jwt),
                                          error));
    if (!iat)
      throw server_error ();

    l3 ([&]{trace << "installation_access_token { " << *iat << " }";});

    // While it would have been nice to cancel CIs of PRs with this branch as
    // base not to waste resources, there are complications: Firstly, we can
    // only do this for remote PRs (since local PRs will most likely share the
    // result with branch push). Secondly, we try to do our best even if the
    // branch protection rule for head behind is not enabled. In this case, it
    // would be good to complete the CI. So maybe/later. See also the head
    // case in handle_pull_request(), where we do cancel remote PRs that are
    // not shared.

    // Service id that uniquely identifies the CI tenant.
    //
    string sid (ps.repository.node_id + ':' + ps.after);

    service_data sd (warning_success,
                     iat->token,
                     iat->expires_at,
                     ps.app_id,
                     ps.installation.id,
                     move (ps.repository.node_id),
                     move (ps.repository.clone_url),
                     service_data::local,
                     false /* pre_check */,
                     false /* re_requested */,
                     report_mode::undetermined,
                     ps.after /* check_sha */,
                     ps.after /* report_sha */);

    // Create an unloaded CI tenant, doing nothing if one already exists
    // (which could've been created by handle_pull_request() or by us as a
    // result of a push to another branch). Note that the tenant's reference
    // count is incremented in all cases.
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
    if (!create (error, warn, verb_ ? &trace : nullptr,
                 *build_db_, retry_max_,
                 tenant_service (sid, "ci-github", sd.json ()),
                 chrono::seconds (15) /* interval */,
                 chrono::seconds (0) /* delay */,
                 duplicate_tenant_mode::ignore))
    {
      fail << "push " + ps.after + " to " + ps.ref
           << ": unable to create unloaded CI tenant";
    }

    return true;
  }

  // Miscellaneous pull request facts
  //
  // - Although some of the GitHub documentation makes it sound like they
  //   expect check runs to be added to both the PR head commit and the merge
  //   commit, the PR UI does not react to the merge commit's check runs
  //   consistently. It actually seems to be quite broken. The only thing it
  //   does seem to do reliably is blocking the PR merge if the merge commit's
  //   check runs are not successful (i.e, overriding the PR head commit's
  //   check runs). But the UI looks quite messed up generally in this state.
  //
  // - When new commits are added to a PR base branch, pull_request.base.sha
  //   does not change, but the test merge commit will be updated to include
  //   the new commits to the base branch.
  //
  // - When new commits are added to a PR head branch, pull_request.head.sha
  //   gets updated with the head commit's SHA and check_suite.pull_requests[]
  //   will contain all PRs with this branch as head.
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
    optional<string> jwt (generate_jwt (pr.pull_request.app_id, trace, error));
    if (!jwt)
      throw server_error ();

    optional<gh_installation_access_token> iat (
      obtain_installation_access_token (pr.installation.id,
                                        move (*jwt),
                                        error));
    if (!iat)
      throw server_error ();

    l3 ([&]{trace << "installation_access_token { " << *iat << " }";});

    // Distinguish between local and remote PRs by comparing the head and base
    // repositories' paths.
    //
    service_data::kind_type kind (
      pr.pull_request.head_path == pr.pull_request.base_path
      ? service_data::local
      : service_data::remote);

    // Note that similar to the branch push case above, while it would have
    // been nice to cancel the previous CI job once the PR head moves (the
    // "synchronize" event), due to the head sharing problem the previous CI
    // job might actually still be relevant (in both local and remote PR
    // cases). So we only do it for the remote PRs and only if the head is not
    // shared (via tenant reference counting).
    //
    if (kind == service_data::remote && pr.action == "synchronize")
    {
      if (pr.before)
      {
        // Service id that will uniquely identify the CI tenant.
        //
        string sid (pr.repository.node_id + ':' + *pr.before);

        if (optional<tenant_service> ts = cancel (error, warn,
                                                  verb_ ? &trace : nullptr,
                                                  *build_db_, retry_max_,
                                                  "ci-github", sid,
                                                  true /* ref_count */))
        {
          l3 ([&]{trace << "pull request " << pr.pull_request.node_id
                        << ": attempted to cancel CI of previous head commit"
                        << " (ref_count: " << ts->ref_count << ')';});
        }
        else
        {
          // It's possible that there was no CI for the previous commit for
          // various reasons (e.g., CI was not enabled).
          //
          l3 ([&]{trace << "pull request " << pr.pull_request.node_id
                        << ": failed to cancel CI of previous head commit "
                        << "with tenant_service id " << sid;});
        }
      }
      else
      {
        error << "pull request " << pr.pull_request.node_id
              << ": before commit is missing in synchronize event";
      }
    }

    // Note: for remote PRs the check_sha will be set later, in
    // build_unloaded_pre_check(), to test merge commit id.
    //
    string check_sha (kind == service_data::local
                      ? pr.pull_request.head_sha
                      : "");

    // Note that PR rebuilds (re-requested) are handled by
    // handle_check_suite_rerequest().
    //
    // Note that, in the case of a remote PR, GitHub will copy the PR head
    // commit from the head (forked) repository into the base repository. So
    // the check runs must always be added to the base repository, whether the
    // PR is local or remote. The head commit refs are located at
    // refs/pull/<PR-number>/head.
    //
    service_data sd (warning_success,
                     move (iat->token),
                     iat->expires_at,
                     pr.pull_request.app_id,
                     pr.installation.id,
                     move (pr.repository.node_id),
                     move (pr.repository.clone_url),
                     kind, true /* pre_check */, false /* re_request */,
                     report_mode::undetermined,
                     move (check_sha),
                     move (pr.pull_request.head_sha) /* report_sha */,
                     pr.pull_request.node_id,
                     pr.pull_request.number);

    // Create an unloaded CI tenant for the pre-check phase (during which we
    // wait for the PR's merge commit and behindness to become available).
    //
    // Create with an empty service id so that the generated tenant id is used
    // instead during the pre-check phase (so as not to clash with a proper
    // service id for this head commit, potentially created in
    // handle_branch_push() or as another PR).
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
                 *build_db_, retry_max_,
                 move (ts),
                 chrono::seconds (15) /* interval */,
                 chrono::seconds (0) /* delay */))
    {
      fail << "pull request " << pr.pull_request.node_id
           << ": unable to create unloaded pre-check tenant";
    }

    return true;
  }

  bool ci_github::
  handle_check_suite_rerequest (gh_check_suite_event cs, bool warning_success)
  {
    HANDLER_DIAG;

    l3 ([&]{trace << "check_suite event { " << cs << " }";});

    assert (cs.action == "rerequested");

    // While we don't need the installation access token in this request,
    // let's obtain it to flush out any permission issues early. Also, it is
    // valid for an hour so we will most likely make use of it.
    //
    optional<string> jwt (generate_jwt (cs.check_suite.app_id, trace, error));
    if (!jwt)
      throw server_error ();

    optional<gh_installation_access_token> iat (
        obtain_installation_access_token (cs.installation.id,
                                          move (*jwt),
                                          error));
    if (!iat)
      throw server_error ();

    l3 ([&]{trace << "installation_access_token { " << *iat << " }";});

    // Service id that uniquely identifies the CI tenant.
    //
    string sid (cs.repository.node_id + ':' + cs.check_suite.head_sha);

    // If the user requests a rebuild of the (entire) PR, then this manifests
    // as the check_suite rather than pull_request event. Specifically:
    //
    // - For a local PR, this event is shared with the branch push and
    //   therefore the check sha is also the report sha (check suite head
    //   sha).
    //
    // - For a remote PR, this event will have no gh_check_suite::head_branch.
    //   In this case the check sha represents the test merge commit and thus
    //   differs from the report sha (check suite head sha).
    //
    //   Note that it's possible the base branch has moved in the meantime and
    //   ideally we would want to re-request the test merge commit, etc.
    //   However, this will only be necessary if the user does not follow our
    //   recommendation of enabling the head-behind-base protection. And it
    //   seems all this extra complexity would not be warranted.
    //

    // Load the service data in order to copy the service data kind, the check
    // sha (in order to cover both the local and remote PR cases described
    // above), and the previous reporting mode (required in build_queued() to
    // decide on the new mode) into the new tentant's service data.
    //
    service_data::kind_type kind;
    string check_sha;
    report_mode rmode;

    if (optional<tenant_data> d = find (*build_db_, "ci-github", sid))
    {
      tenant_service& ts (d->service);

      try
      {
        service_data sd (*ts.data);

        kind = sd.kind;
        check_sha = move (sd.check_sha);
        rmode = sd.report_mode;
      }
      catch (const invalid_argument& e)
      {
        fail << "failed to parse service data: " << e;
      }
    }
    else
    {
      error << "check suite " << cs.check_suite.node_id
            << " re-requested but tenant_service with id " << sid
            << " did not exist";
      return true;
    }

    // Sanity check the local case (see above for details).
    //
    if (kind == service_data::local)
    {
      assert (cs.check_suite.head_branch.has_value ());
      assert (check_sha == cs.check_suite.head_sha);
    }

    service_data sd (warning_success,
                     iat->token,
                     iat->expires_at,
                     cs.check_suite.app_id,
                     cs.installation.id,
                     move (cs.repository.node_id),
                     move (cs.repository.clone_url),
                     kind, false /* pre_check */, true /* re_requested */,
                     rmode,
                     move (check_sha),
                     move (cs.check_suite.head_sha) /* report_sha */);

    // Replace the existing CI tenant if it exists.
    //
    // Note that GitHub UI does not allow re-running the entire check suite
    // until all the check runs are completed. However if we got here as a
    // result of re-requesting the check suite from build_canceled() the check
    // runs could be in any state (and, yes, re-requesting a not completed
    // check suite via the API works).

    // Create an unloaded CI tenant.
    //
    // Impose a delay to avoid GitHub state update races (see build_cancel()
    // for background). @@ Should also help prevent abuse, though the delay
    // should probably be longer (and depend on when was the last time it was
    // re-requested, similar to what the build_force module does). @@ TODO:
    // also update diagnostics handle_forced_check_suite_rebuild().
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
                     *build_db_, retry_max_,
                     tenant_service (sid, "ci-github", sd.json ()),
                     chrono::seconds (15) /* interval */,
                     chrono::seconds (60) /* delay */,
                     duplicate_tenant_mode::replace));

    if (!pr)
    {
      fail << "check suite " << cs.check_suite.node_id
           << ": unable to create unloaded CI tenant";
    }

    if (pr->second == duplicate_tenant_result::created)
    {
      error << "check suite " << cs.check_suite.node_id
            << ": re-requested but tenant_service with id " << sid
            << " did not exist";
      return true;
    }

    // Re-create a temporary conclusion check run in the queued state to
    // provide immediate user feedback (the real conclusion check run is only
    // re-created when the tenant is loaded).
    //
    // Note that we cannot provide a details URL because the tenant id is not
    // readily available.
    //
    // Note also that we do it after replacing the tenant to make sure it is
    // done without delay (see build_cancel() for background).
    //
    auto create_ccr = [this, &error, &sd, iat] (const string& summary)
    {
      check_run cr;
      cr.name = conclusion_check_run_name (sd.app_id);

      if (!gq_create_check_run (
            error,
            cr,
            iat->token,
            sd.app_id, sd.repository_node_id, sd.report_sha,
            nullopt /* details_url */,
            build_state::queued, check_run_queued_title,
            summary + ' ' + force_rebuild_md_link (sd) + '.'))
      {
        error << "failed to re-create conclusion check run";
      }
    };

    create_ccr ("Rebuild initiated, waiting for the builds to restart.");

    return true;
  }

  bool ci_github::
  handle_check_suite_completed (gh_check_suite_event cs, bool warning_success)
  {
    // The plans is as follows:
    //
    // 1. Load the service data.
    //
    // 2. Verify it is completed.
    //
    // 3. Verify the check run counts match.
    //
    // 4. Verify (like in build_built()) that all the check runs are
    //    completed.
    //
    // 5. Verify the result matches what GitHub thinks it is.

    HANDLER_DIAG;

    l3 ([&]{trace << "check_suite event { " << cs << " }";});

    // Service id that uniquely identifies the CI tenant.
    //
    string sid (cs.repository.node_id + ':' + cs.check_suite.head_sha);

    // The common log entry subject.
    //
    string sub ("check suite " + cs.check_suite.node_id + '/' + sid);

    // Load the service data.
    //
    service_data sd;

    if (optional<tenant_data> d = find (*build_db_, "ci-github", sid))
    {
      try
      {
        sd = service_data (*d->service.data);
      }
      catch (const invalid_argument& e)
      {
        fail << "failed to parse service data: " << e;
      }
    }
    else
    {
      error << sub << ": tenant_service does not exist";
      return true;
    }

    // Verify the completed flag and the number of check runs.
    //
    if (!sd.completed)
    {
      error << sub << " service data complete flag is false";
      return true;
    }

    // Received count will be one higher because we don't store the conclusion
    // check run.
    //
    size_t check_runs_count (sd.check_runs.size () + 1);

    if (check_runs_count == 1)
    {
      error << sub << ": no check runs in service data";
      return true;
    }

    // In the aggregate reporting mode there won't be any check runs on
    // GitHub. It's also theoretically possible for the reporting mode to be
    // undetermined at this stage in which case all check runs would not have
    // been created (see build_built()).
    //
    if (sd.report_mode == report_mode::detailed)
    {
      if (cs.check_suite.check_runs_count != check_runs_count)
      {
        error << sub << ": check runs count " << cs.check_suite.check_runs_count
              << " does not match service data count " << check_runs_count;
        return true;
      }
    }

    // Verify that all the check runs are built and compute the summary
    // conclusion.
    //
    result_status conclusion (result_status::success);

    for (const check_run& cr: sd.check_runs)
    {
      if (cr.state == build_state::built)
      {
        assert (cr.status.has_value ());
        conclusion |= *cr.status;
      }
      else
      {
        error << sub << ": unbuilt check run in service data";
        return true;
      }
    }

    // Verify the conclusion.
    //
    if (!cs.check_suite.conclusion)
    {
      error << sub << ": absent conclusion in completed check suite";
      return true;
    }

    // Note that the case mismatch is due to GraphQL (gh_conclusion())
    // requiring uppercase conclusion values while the received webhook values
    // are lower case.
    //
    string gh_conclusion (gh_to_conclusion (conclusion, warning_success));

    if (icasecmp (*cs.check_suite.conclusion, gh_conclusion) != 0)
    {
      error << sub << ": conclusion " << *cs.check_suite.conclusion
            << " does not match service data conclusion " << gh_conclusion;
      return true;
    }

    return true;
  }

  // Make a check run summary from a CI start_result.
  //
  static string
  to_check_run_summary (const optional<ci_start::start_result>& r)
  {
    string s;

    s = "```\n";
    if (r) s += r->message;
    else s += "Internal service error.";
    s += "\n```";

    return s;
  }

  // Create a gq_built_result.
  //
  // Throw invalid_argument in case of invalid result_status.
  //
  static gq_built_result
  make_built_result (result_status rs, bool warning_success, string message)
  {
    string title (circle (rs == result_status::warning && !warning_success
                          ? result_status::error
                          : rs));
    title += ' ';
    title += ucase (to_string (rs));

    return {gh_to_conclusion (rs, warning_success),
            move (title),
            move (message)};
  }

  // Parse a check run details URL into a build_id.
  //
  // Return nullopt if the URL is invalid.
  //
  static optional<build_id>
  parse_details_url (const string& details_url);

  // Note that GitHub always posts a message to their GUI saying "You have
  // successfully requested <check_run_name> be rerun", regardless of what
  // HTTP status code we respond with. However we do return error status codes
  // when there is no better option (like failing the conclusion) in case they
  // start handling them someday.
  //
  bool ci_github::
  handle_check_run_rerequest (const gh_check_run_event& cr,
                              bool warning_success)
  {
    HANDLER_DIAG;

    l3 ([&]{trace << "check_run event { " << cr << " }";});

    // Get a new installation access token.
    //
    auto get_iat = [this, &trace, &error, &cr] ()
      -> optional<gh_installation_access_token>
    {
      optional<string> jwt (generate_jwt (cr.check_run.app_id, trace, error));
      if (!jwt)
        return nullopt;

      optional<gh_installation_access_token> iat (
        obtain_installation_access_token (cr.installation.id,
                                          move (*jwt),
                                          error));

      if (iat)
        l3 ([&]{trace << "installation_access_token { " << *iat << " }";});

      return iat;
    };

    // The overall plan is as follows:
    //
    // 1. Load service data.
    //
    // 2. If the tenant is archived, then fail (re-create) both the check run
    //    and the conclusion with appropriate diagnostics.
    //
    // 3. If the check run is in the queued state, then do nothing.
    //
    // 4. Re-create the check run in the queued state and the conclusion in
    //    the building state. Note: do in a single request to make sure we
    //    either "win" or "loose" the potential race for both (important
    //    for #7).
    //
    // 5. Call the rebuild() function to attempt to schedule a rebuild. Pass
    //    the update function that does the following (if called):
    //
    //    a. Save new node ids.
    //
    //    b. Update the check run state (may also not exist).
    //
    //    c. Clear the completed flag if true.
    //
    // 6. If the result of rebuild() indicates the tenant is archived, then
    //    fail (update) both the check run and conclusion with appropriate
    //    diagnostics.
    //
    // 7. If original state is queued (no rebuild was scheduled), then fail
    //    (update) both the check run and the conclusion.
    //
    // Note that while conceptually we are updating existing check runs, in
    // practice we have to re-create as new check runs in order to replace the
    // existing ones because GitHub does not allow transitioning out of the
    // built state.

    // Ignore if this is the conclusion check run (see below for why we cannot
    // fail, but in a nutshell, a request to update all the failed check runs
    // will always include the conclusion).
    //
    // Note that we should check this early since parse_details_url() for it
    // will fail.
    //
    if (cr.check_run.name.compare (0,
                                   conclusion_check_run_basename.size (),
                                   conclusion_check_run_basename) == 0)
    {
      l3 ([&]{trace << "re-requested conclusion check_run";});
      return true;
    }

    // Parse the check_run's details_url to extract build id.
    //
    // While this is a bit hackish, there doesn't seem to be a better way
    // (like associating custom data with a check run). Note that the GitHub
    // UI only allows rebuilding completed check runs, so the details URL
    // should be there.
    //
    optional<build_id> bid (parse_details_url (cr.check_run.details_url));
    if (!bid)
    {
      fail << "check run " << cr.check_run.node_id
           << ": failed to extract build id from details_url";
    }

    const string& repo_node_id (cr.repository.node_id);
    const string& head_sha (cr.check_run.check_suite.head_sha);

    // Prepare the build and conclusion check runs. They are sent to GitHub in
    // a single request (unless something goes wrong) so store them together
    // from the outset.
    //
    brep::check_runs check_runs (2);
    check_run& bcr (check_runs[0]); // Build check run
    check_run& ccr (check_runs[1]); // Conclusion check run

    ccr.name = conclusion_check_run_name (cr.check_run.app_id);

    const gh_installation_access_token* iat (nullptr);
    optional<gh_installation_access_token> new_iat;

    // Load the service data, failing the check runs if the tenant has been
    // archived.
    //
    service_data sd;
    string tenant_id;
    {
      // Service id that uniquely identifies the CI tenant.
      //
      string sid (repo_node_id + ':' + head_sha);

      optional<tenant_data> d (find (*build_db_, "ci-github", sid));
      if (!d)
      {
        // No such tenant.
        //
        fail << "check run " << cr.check_run.node_id
             << " re-requested but tenant_service with id " << sid
             << " does not exist";
      }

      tenant_service& ts (d->service);

      try
      {
        sd = service_data (*ts.data);
      }
      catch (const invalid_argument& e)
      {
        fail << "failed to parse service data: " << e;
      }

      tenant_id = move (d->tenant_id);

      // It's possible that the tenant has been re-created due to a large
      // number of rebuild requests (see build_canceled()). So we ignore
      // requests for the (presumably) old tenant.
      //
      if (tenant_id != bid->package.tenant)
      {
        l3 ([&]{trace << "tenant id mismatch, old: " << bid->package.tenant
                      << ", new: " << tenant_id;});

        return true;
      }

      if (!sd.conclusion_node_id)
        fail << "no conclusion node id for check run " << cr.check_run.node_id;

      // Get a new IAT if the one from the service data has expired.
      //
      if (system_clock::now () > sd.installation_access.expires_at)
      {
        if ((new_iat = get_iat ()))
          iat = &*new_iat;
        else
          throw server_error ();
      }
      else
        iat = &sd.installation_access;

      if (d->archived) // Tenant is archived
      {
        // Fail (update) the check runs.
        //
        gq_built_result br (
          make_built_result (
            result_status::error, warning_success,
            "Unable to rebuild individual configuration: build has "
            "been archived."));

        // Update the build check run.
        //
        // Try to update the conclusion check run even if the first update
        // fails.
        //
        bool f (false); // Failed.

        if (gq_update_check_run (error, bcr, iat->token,
                                 repo_node_id, cr.check_run.node_id,
                                 br))
        {
          l3 ([&]{trace << "updated check_run { " << bcr << " }";});
        }
        else
        {
          error << "check_run " << cr.check_run.node_id
                << ": unable to update check run";
          f = true;
        }

        // Update the conclusion check run.
        //
        // Append the force rebuild link to the summary.
        //
        br.summary += ' ' + force_rebuild_md_link (sd) + '.';

        if (gq_update_check_run (error, ccr, iat->token,
                                 repo_node_id, *sd.conclusion_node_id,
                                 move (br)))
        {
          l3 ([&]{trace << "updated conclusion check_run { " << ccr << " }";});
        }
        else
        {
          error << "check_run " << cr.check_run.node_id
                << ": unable to update conclusion check run";
          f = true;
        }

        // Fail the handler if either of the check runs could not be
        // updated.
        //
        if (f)
          throw server_error ();

        return true;
      }
    }

    // Note: handled at the beginning of the function.
    //
#if 0
    // Fail if it's the conclusion check run that is being re-requested.
    //
    // Expect that if the user selects re-run all failed checks we will
    // receive multiple check runs, one of which will be the conclusion. And
    // if we fail it while it happens to arrive last, then we will end up in
    // the wrong overall state (real check run is building while conclusion is
    // failed). It seems the best we can do is to ignore it: if the user did
    // request a rebuild of the conclusion check run explicitly, there will be
    // no change, which is not ideal but is still an indication that this
    // operation is not supported.
    //
    if (cr.check_run.name.compare (0,
                                   conclusion_check_run_basename.size (),
                                   conclusion_check_run_basename) == 0)
    {
      l3 ([&]{trace << "re-requested conclusion check_run";});

      if (!sd.conclusion_node_id)
        fail << "no conclusion node id for check run " << cr.check_run.node_id;

      gq_built_result br (
        make_built_result (result_status::error, warning_success,
                           "Conclusion check run cannot be rebuilt"));

      // Fail (update) the conclusion check run.
      //
      if (gq_update_check_run (error, ccr, iat->token,
                               repo_node_id, *sd.conclusion_node_id,
                               move (br)))
      {
        l3 ([&]{trace << "updated conclusion check_run { " << ccr << " }";});
      }
      else
      {
        fail << "check run " << cr.check_run.node_id
             << ": unable to update conclusion check run "
             << *sd.conclusion_node_id;
      }

      return true;
    }
#endif

    // Initialize the check run (`bcr`) with state from the service data.
    //
    {
      // Search for the check run in the service data.
      //
      // Note that we look by name in case node id got replaced by a racing
      // re-request (in which case we ignore this request).
      //
      auto i (find_if (sd.check_runs.begin (), sd.check_runs.end (),
                       [&cr] (const check_run& scr)
                       {
                         return scr.name == cr.check_run.name;
                       }));

      if (i == sd.check_runs.end ())
        fail << "check_run " << cr.check_run.node_id
             << " (" << cr.check_run.name << "): "
             << "re-requested but does not exist in service data";

      // Do nothing if node ids don't match.
      //
      if (i->node_id && *i->node_id != cr.check_run.node_id)
      {
        l3 ([&]{trace << "check_run " << cr.check_run.node_id
                      << " (" << cr.check_run.name << "): "
                      << "node id has changed in service data";});
        return true;
      }

      // Do nothing if the build is already queued.
      //
      if (i->state == build_state::queued)
      {
        l3 ([&]{trace << "ignoring already-queued check run";});
        return true;
      }

      bcr.name = i->name;
      bcr.build_id = i->build_id;
      bcr.state = i->state;
    }

    // Transition the build and conclusion check runs out of the built state
    // (or any other state) by re-creating them.
    //
    bcr.state = build_state::queued;
    bcr.state_synced = false;
    bcr.details_url = cr.check_run.details_url;
    bcr.description = {check_run_queued_title, check_run_queued_summary};

    ccr.state = build_state::building;
    ccr.state_synced = false;
    ccr.details_url = details_url (tenant_id);
    ccr.description = {conclusion_building_title,
                       conclusion_building_summary +
                       ' ' + force_rebuild_md_link (sd) + '.'};

    if (gq_create_check_runs (error, check_runs, iat->token,
                              cr.check_run.app_id, repo_node_id, head_sha,
                              options_->build_queued_batch ()))
    {
      assert (bcr.state == build_state::queued);
      assert (ccr.state == build_state::building);

      l3 ([&]{trace << "created check_run { " << bcr << " }";});
      l3 ([&]{trace << "created conclusion check_run { " << ccr << " }";});
    }
    else
    {
      fail << "check run " << cr.check_run.node_id
           << ": unable to re-create build and conclusion check runs";
    }

    // Request the rebuild and update service data.
    //
    bool race (false);

    // Callback function called by rebuild() to update the service data (but
    // only if the build is actually restarted).
    //
    auto update_sd = [&error, &new_iat, &race,
                      tenant_id = move (tenant_id),
                      &cr, &bcr, &ccr] (const string& ti,
                                        const tenant_service& ts,
                                        build_state) -> optional<string>
    {
      // NOTE: this lambda may be called repeatedly (e.g., due to transaction
      // being aborted) and so should not move out of its captures.

      race = false; // Reset.

      if (tenant_id != ti)
      {
        // The tenant got replaced since we loaded it but we managed to
        // trigger a rebuild in the new tenant. Who knows whose check runs are
        // visible, so let's fail ours similar to the cases below.
        //
        race = true;
        return nullopt;
      }

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

      // Note that we again look by name in case node id got replaced by a
      // racing re-request. In this case, however, it's impossible to decide
      // who won that race, so let's fail the check suite to be on the safe
      // side (in a sense, similar to the rebuild() returning queued below).
      //
      auto i (find_if (
                sd.check_runs.begin (), sd.check_runs.end (),
                [&cr] (const check_run& scr)
                {
                  return scr.name == cr.check_run.name;
                }));

      if (i == sd.check_runs.end ())
      {
        error << "check_run " << cr.check_run.node_id
              << " (" << cr.check_run.name << "): "
              << "re-requested but does not exist in service data";
        return nullopt;
      }

      if (i->node_id && *i->node_id != cr.check_run.node_id)
      {
        // Keep the old conclusion node id to make sure any further state
        // transitions are ignored. A bit of a hack.
        //
        race = true;
        return nullopt;
      }

      *i = bcr; // Update with new node_id, state, state_synced.

      sd.conclusion_node_id = ccr.node_id;
      sd.completed = false;

      // Save the IAT if we created a new one.
      //
      if (new_iat)
        sd.installation_access = *new_iat;

      return sd.json ();
    };

    optional<build_state> bs (
      rebuild (*build_db_, retry_max_,
               tenant_service_map_,
               log_writer_,
               *bid, update_sd));

    // If the build has been archived or re-enqueued since we loaded the
    // service data, fail (by updating) both the build check run and the
    // conclusion check run. Otherwise the build has been successfully
    // re-enqueued so do nothing further.
    //
    if (!race && bs && *bs != build_state::queued)
      return true;

    gq_built_result br; // Built result for both check runs.

    if (race || bs) // Race or re-enqueued.
    {
      // The re-enqueued case: this build has been re-enqueued since we first
      // loaded the service data. This could happen if the user clicked
      // "re-run" multiple times and another handler won the rebuild() race.
      //
      // However the winner of the check runs race cannot be determined.
      //
      // Best case the other handler won the check runs race as well and
      // thus everything will proceed normally. Our check runs will be
      // invisible and disregarded.
      //
      // Worst case we won the check runs race and the other handler's check
      // runs -- the ones that will be updated by the build_*() notifications
      // -- are no longer visible, leaving things quite broken.
      //
      // Either way, we fail our check runs. In the best case scenario it
      // will have no effect; in the worst case scenario it lets the user
      // know something has gone wrong.
      //
      br = make_built_result (result_status::error, warning_success,
                              "Unable to rebuild, try again");
    }
    else // Archived.
    {
      // The build has expired since we loaded the service data. Most likely
      // the tenant has been archived.
      //
      br = make_built_result (
        result_status::error, warning_success,
        "Unable to rebuild individual configuration: build has been archived.");
    }

    // Try to update the conclusion check run even if the first update fails.
    //
    bool f (false); // Failed.

    // Fail the build check run.
    //
    if (gq_update_check_run (error, bcr, iat->token,
                             repo_node_id, *bcr.node_id,
                             br))
    {
      l3 ([&]{trace << "updated check_run { " << bcr << " }";});
    }
    else
    {
      error << "check run " << cr.check_run.node_id
            << ": unable to update (replacement) check run "
            << *bcr.node_id;
      f = true;
    }

    // Fail the conclusion check run.
    //
    // Append the force rebuild link to the summary.
    //
    br.summary += ' ' + force_rebuild_md_link (sd) + '.';

    if (gq_update_check_run (error, ccr, iat->token,
                             repo_node_id, *ccr.node_id,
                             move (br)))
    {
      l3 ([&]{trace << "updated conclusion check_run { " << ccr << " }";});
    }
    else
    {
      error << "check run " << cr.check_run.node_id
            << ": unable to update conclusion check run " << *ccr.node_id;
      f = true;
    }

    // Fail the handler if either of the check runs could not be updated.
    //
    if (f)
      throw server_error ();

    return true;
  }

  bool ci_github::
  handle_forced_check_suite_rebuild (const name_values& rps, response& rs)
  {
    HANDLER_DIAG;

    // Process the request query parameters.
    //
    string repo_id;
    string head_sha;
    string reason;
    {
      auto badreq = [] (const string& m)
      {
        throw invalid_request (400, m);
      };

      for (const name_value& rp: rps)
      {
        auto c = [badreq, &rp] (const char* n)
        {
          if (rp.name == n)
          {
            if (!rp.value)
              badreq ("missing '" + string (n) + "' parameter value");

            return true;
          }
          return false;
        };

        if      (c ("repo-id"))  repo_id = *rp.value;
        else if (c ("head-sha")) head_sha = *rp.value;
        else if (c ("reason"))   reason = *rp.value;
      }

      if (repo_id.empty ()) badreq ("missing 'repo-id' parameter");
      if (head_sha.empty ()) badreq ("missing 'head-sha' parameter");
      if (reason.empty ()) badreq ("missing rebuild reason"); // User-visible.
    }

    string sid (repo_id + ':' + head_sha);

    // Log the force rebuild with the warning severity, truncating the
    // reason if too long.
    //
    {
      diag_record dr (warn);
      dr << "force rebuild for " << sid << ": ";

      if (reason.size () < 50)
        dr << reason;
      else
        dr << string (reason, 0, 50) << "...";
    }

    // Load the service data.
    //
    service_data sd;

    if (optional<tenant_data> d = find (*build_db_, "ci-github", sid))
    {
      tenant_service& ts (d->service);

      try
      {
        sd = service_data (*ts.data);
      }
      catch (const invalid_argument& e)
      {
        fail << "failed to parse service data: " << e;
      }
    }
    else
      throw invalid_request (400,
                             "no build for repository id: " + repo_id +
                             ", commit id: " + head_sha); // User-visible.

    // Get a new installation access token if the current one has expired.
    //
    const gh_installation_access_token* iat (nullptr);
    optional<gh_installation_access_token> new_iat;

    if (system_clock::now () > sd.installation_access.expires_at)
    {
      optional<string> jwt (generate_jwt (sd.app_id, trace, error));
      if (!jwt)
        throw server_error ();

      new_iat = obtain_installation_access_token (sd.installation_id,
                                                  move (*jwt),
                                                  error);
      if (!new_iat)
        throw server_error ();

      iat = &*new_iat;
    }
    else
      iat = &sd.installation_access;

    // Re-request the check suite.

    // Note that the service id remains valid across tenant recreation (and
    // thus so does the force rebuild URL) so there may well not be a check
    // suite node id for the current tenant yet. Feels like ignoring the
    // request is the most sensible option (the tenant is presumably being
    // created/loaded).
    //
    const char* r (nullptr);
    if (sd.check_suite_node_id)
    {
      const string& nid (*sd.check_suite_node_id);

      if (gq_rerequest_check_suite (error,
                                    iat->token,
                                    sd.repository_node_id,
                                    nid))
      {
        l3 ([&]{trace << "re-requested check suite " << nid;});
        r = "Rebuilding in 60 seconds."; // @@ TODO: dynamic delay.
      }
      else
        fail << "failed to re-request check suite " << nid;
    }
    else
      r = "Rebuild already in progress.";

    // We have all the data, so don't buffer the response content.
    //
    ostream& os (rs.content (200, "text/plain;charset=utf-8", false));
    os << r;

    return true;
  }

  string ci_github::
  conclusion_check_run_name (uint64_t app_id) const
  {
    const map<uint64_t, string>& an (options_->ci_github_app_id_name ());

    auto ni (an.find (app_id));
    if (ni == an.end ())
      throw invalid_argument ("no app name configured for app id " +
                              to_string (app_id));

    return conclusion_check_run_basename + " (" + ni->second + ')';
  }

  function<optional<string> (const string&, const tenant_service&)> ci_github::
  build_unloaded (const string& ti,
                  tenant_service&& ts,
                  const diag_epilogue& log_writer) const noexcept
  {
    // NOTE: this function is noexcept and should not throw.

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
      : build_unloaded_load (ti, move (ts), move (sd), log_writer);
  }

  function<optional<string> (const string&, const tenant_service&)> ci_github::
  build_unloaded_pre_check (tenant_service&& ts,
                            service_data&& sd,
                            const diag_epilogue& log_writer) const noexcept
  try
  {
    // NOTE: this function is noexcept and should not throw.
    //
    // In a few places where invalid_argument is unlikely to be thrown and/or
    // would indicate that things are seriously broken we let it propagate to
    // the function catch block where the pre-check tenant will be canceled
    // (otherwise we could end up in an infinite loop, e.g., because the
    // problematic arguments won't change).

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
    // Let unlikely invalid_argument propagate (see above).
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
      // Create the CI tenant by reusing the pre-check service data.
      //
      sd.pre_check = false;

      // Set the service data's check_sha if this is a remote PR. The test
      // merge commit refs are located at refs/pull/<PR-number>/merge.
      //
      if (sd.kind == service_data::remote)
        sd.check_sha = *pc->merge_commit_sha;

      // Service id that will uniquely identify the CI tenant.
      //
      string sid (sd.repository_node_id + ':' + sd.report_sha);

      // Create an unloaded CI tenant, doing nothing if one already exists
      // (which could've been created by a head branch push or another PR
      // sharing the same head commit). Note that the tenant's reference count
      // is incremented in all cases.
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
      try
      {
        if (auto pr = create (error, warn, verb_ ? &trace : nullptr,
                              *build_db_, retry_max_,
                              tenant_service (sid, "ci-github", sd.json ()),
                              chrono::seconds (15) /* interval */,
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
            // local PR, or another remote PR) which in turn means the CI
            // result may end up being for head, not merge commit. There is
            // nothing we can do about it on our side (the user can enable the
            // head-behind-base protection on their side).
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
                << ": failed to create unloaded CI tenant "
                << "with tenant_service id " << sid;

          // Fall through to cancel.
        }
      }
      catch (const runtime_error& e) // Database retries exhausted.
      {
        error << "pull request " << *sd.pr_node_id
              << ": failed to create unloaded CI tenant "
              << "with tenant_service id " << sid
              << ": " << e.what ();

        // Fall through to cancel.
      }
    }

    // Cancel the pre-check tenant.
    //
    try
    {
      if (!cancel (error, warn, verb_ ? &trace : nullptr,
                   *build_db_, retry_max_,
                   ts.type,
                   ts.id))
      {
        // Should never happen (no such tenant).
        //
        error << "pull request " << *sd.pr_node_id
              << ": failed to cancel pre-check tenant with tenant_service id "
              << ts.id;
      }
    }
    catch (const runtime_error& e) // Database retries exhausted.
    {
      error << "pull request " << *sd.pr_node_id
            << ": failed to cancel pre-check tenant with tenant_service id "
            << ts.id << ": " << e.what ();
    }

    return nullptr;
  }
  catch (const std::exception& e)
  {
    NOTIFICATION_DIAG (log_writer);
    error << "pull request " << *sd.pr_node_id
          << ": unhandled exception: " << e.what ();

    // Cancel the pre-check tenant otherwise we could end up in an infinite
    // loop (see top of function).
    //
    try
    {
      if (cancel (error, warn, verb_ ? &trace : nullptr,
                  *build_db_, retry_max_,
                  ts.type,
                  ts.id))
        l3 ([&]{trace << "canceled pre-check tenant " << ts.id;});
    }
    catch (const runtime_error& e) // Database retries exhausted.
    {
      l3 ([&]{trace << "failed to cancel pre-check tenant " << ts.id << ": "
                    << e.what ();});
    }

    return nullptr;
  }

  static ostream&
  operator<< (ostream& os, const gq_rate_limits& l)
  {
    if (l.reset != butl::timestamp_unknown)
    {
      using butl::operator<<; // For timestamp (l.reset).

      os << "{ limit: "      << l.limit
         <<  ", remaining: " << l.remaining
         <<  ", used: "      << l.used
         <<  ", reset: "     << l.reset
         << " }";
    }
    else
      os << "<unknown>";

    return os;
  }

  function<optional<string> (const string&, const tenant_service&)> ci_github::
  build_unloaded_load (const string& tenant_id,
                       tenant_service&& ts,
                       service_data&& sd,
                       const diag_epilogue& log_writer) const noexcept
  try
  {
    // NOTE: this function is noexcept and should not throw.
    //
    // In a few places where invalid_argument is unlikely to be thrown and/or
    // would indicate that things are seriously broken we let it propagate to
    // the function catch block where the tenant will be canceled (otherwise
    // we could end up in an infinite loop, e.g., because the problematic
    // arguments won't change).

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
      if (optional<string> jwt = generate_jwt (sd.app_id, trace, error))
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

    optional<string> check_suite_node_id;

    // Create the synthetic conclusion check run with an in-progress
    // state. Return the check run on success or nullopt on failure.
    //
    gq_rate_limits limits;
    auto create_ccr = [&tenant_id,
                       iat,
                       &check_suite_node_id,
                       &limits,
                       &sd,
                       &error,
                       this] (const string& title,
                              const string& summary)
      -> optional<check_run>
    {
      check_run cr;
      // Let invalid_argument propagate (see above).
      //
      cr.name = conclusion_check_run_name (sd.app_id);

      // Let unlikely invalid_argument propagate (see above).
      //
      if ((check_suite_node_id = gq_create_check_run (error,
                                                      cr,
                                                      iat->token,
                                                      sd.app_id,
                                                      sd.repository_node_id,
                                                      sd.report_sha,
                                                      details_url (tenant_id),
                                                      build_state::building,
                                                      title,
                                                      summary,
                                                      &limits)))
      {
        return cr;
      }
      else
        return nullopt;
    };

    // Update the synthetic conclusion check run with success or failure.
    // Return the check run on success or nullopt on failure.
    //
    auto update_ccr = [this,
                       iat,
                       &sd,
                       &error] (const string& node_id,
                                result_status rs,
                                string summary) -> optional<check_run>
    {
      assert (!node_id.empty ());

      // Let unlikely invalid_argument propagate (see above).
      //
      gq_built_result br (
        make_built_result (rs, sd.warning_success, move (summary)));

      check_run cr;
      // Let invalid_argument propagate (see above).
      //
      cr.name = conclusion_check_run_name (sd.app_id); // Display purposes only.

      // Let unlikely invalid_argument propagate (see above).
      //
      if (gq_update_check_run (error,
                               cr,
                               iat->token,
                               sd.repository_node_id,
                               node_id,
                               move (br)))
      {
        assert (cr.state == build_state::built);
        return cr;
      }
      else
        return nullopt;
    };

    // (Re)create the synthetic conclusion check run first in order to convert
    // a potentially completed check suite to building as early as possible.
    //
    // Note that there is a window between receipt of a check_suite or
    // pull_request event and the first bot/worker asking for a task, which
    // could be substantial. We could probably (also) try to (re)create the
    // conclusion checkrun in the webhook handler. @@ Maybe/later.
    //
    string conclusion_node_id; // Conclusion check run node ID.
    optional<uint64_t> rb;     // Report budget.

    if (!sd.conclusion_node_id)
    {
      if (auto cr = create_ccr (conclusion_building_title,
                                conclusion_building_summary +
                                ' ' + force_rebuild_md_link (sd) + '.'))
      {
        l3 ([&]{trace << "created check_run { " << *cr << " }";});

        conclusion_node_id = move (*cr->node_id);

        if (limits.reset != timestamp_unknown)
          rb = report_budget (limits, error);
      }

      // Log the limits returned by create_ccr() and budget, if present.
      //
      diag_record dr (info);
      dr << "installation id " << sd.installation_id << " limits: "
         << limits;

      if (rb)
        dr << ", budget: " << *rb;
    }

    const string& effective_conclusion_node_id (
      sd.conclusion_node_id
      ? *sd.conclusion_node_id
      : conclusion_node_id);

    // Load the CI tenant if the conclusion check run was created.
    //
    if (!effective_conclusion_node_id.empty ())
    {
      string ru; // Repository URL.

      // CI the test merge commit for remote PRs and the head commit for
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

      // Let unlikely invalid_argument propagate (see above).
      //
      repository_location rl (move (ru), repository_type::git);

      try
      {
        optional<start_result> r (load (error, warn, verb_ ? &trace : nullptr,
                                        *build_db_, retry_max_,
                                        move (ts),
                                        move (rl)));

        if (!r || r->status != 200)
        {
          string sm (to_check_run_summary (r) +
                     "\n\n" + force_rebuild_md_link (sd) + '.');

          // Let unlikely invalid_argument propagate (see above).
          //
          if (auto cr = update_ccr (effective_conclusion_node_id,
                                    result_status::error,
                                    move (sm)))
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
      catch (const runtime_error& e) // Database retries exhausted.
      {
        error << "failed to load CI tenant " << ts.id << ": " << e.what ();

        // Fall through to retry on next call.
      }
    }

    if (!new_iat && conclusion_node_id.empty ())
      return nullptr; // Nothing to save (but potentially retry on next call).

    return [&error,
            tenant_id,
            rb,
            iat = move (new_iat),
            csi = move (check_suite_node_id),
            cni = move (conclusion_node_id)]
      (const string& ti,
       const tenant_service& ts) -> optional<string>
    {
      // NOTE: this lambda may be called repeatedly (e.g., due to
      // transaction being aborted) and so should not move out of its
      // captures.

      if (tenant_id != ti)
        return nullopt; // Do nothing if the tenant has been replaced.

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

      if (rb)
        sd.report_budget = *rb;

      if (iat)
        sd.installation_access = *iat;

      if (csi)
        sd.check_suite_node_id = *csi;

      if (!cni.empty ())
        sd.conclusion_node_id = cni;

      return sd.json ();
    };
  }
  catch (const std::exception& e)
  {
    NOTIFICATION_DIAG (log_writer);
    error << "CI tenant " << ts.id << ": unhandled exception: " << e.what ();

    // Cancel the tenant otherwise we could end up in an infinite loop (see
    // top of function).
    //
    try
    {
      if (cancel (error, warn, verb_ ? &trace : nullptr,
                  *build_db_, retry_max_, ts.type, ts.id))
        l3 ([&]{trace << "canceled CI tenant " << ts.id;});
    }
    catch (const runtime_error& e) // Database retries exhausted.
    {
      l3 ([&]{trace << "failed to cancel CI tenant " << ts.id
                    << ": " << e.what ();});
    }

    return nullptr;
  }

  // The cumulative statistics for a number of builds.
  //
  struct build_stats
  {
    size_t queued_count = 0;
    size_t building_count = 0;

    // Counts for completed builds.
    //
    // Note that the warning count will be included in the success or failure
    // count (see calculate_build_stats()).
    //
    size_t success_count = 0;
    size_t warning_count = 0;
    size_t failure_count = 0;

    // Aggregated result status. Absent if not all builds have completed.
    //
    optional<result_status> result;
  };

  // Calculate the cumulative statistics for a number of builds.
  //
  // Count the number of occurrences of each build state and calculate an
  // aggregated result status if all builds have completed.
  //
  // Note that the warning count will be included in the success or failure
  // count (depending on the value of warning_success). Thus the total number
  // of builds is the sum of all the counts excluding warnings.
  //
  static build_stats
  calculate_build_stats (const check_runs& crs, bool warning_success)
  {
    build_stats r;

    if (!crs.empty ())
      r.result = result_status::success;

    for (const check_run& cr: crs)
    {
      switch (cr.state)
      {
      case build_state::queued:
        {
          r.result = nullopt;
          ++r.queued_count;
          break;
        }
      case build_state::building:
        {
          r.result = nullopt;
          ++r.building_count;
          break;
        }
      case build_state::built:
        {
          assert (cr.status);

          // Add the result status to the count.
          //
          switch (*cr.status)
          {
          case result_status::success:  ++r.success_count; break;

          case result_status::error:
          case result_status::abort:
          case result_status::abnormal: ++r.failure_count; break;

          case result_status::warning:
            {
              ++r.warning_count;

              // Include the warning count in the success or failure count.
              //
              if (warning_success)
                ++r.success_count;
              else
                ++r.failure_count;

              break;
            }

          case result_status::skip:
          case result_status::interrupt:
            {
              assert (false);
            }
          }

          // Aggregate the result status.
          //
          if (r.result)
            *r.result |= *cr.status;

          break;
        }
      }
    }

    return r;
  }

  // Construct the builds statistics report. For example:
  //
  // 0 queued, 5 building, 3 failed, 10 succeeded (4 with warnings), 18 total
  //
  static string
  make_build_stats_report (const build_stats& bss, bool warning_success)
  {
    ostringstream os;

    // Note that we can omit both or queued, but if we show queued, we also
    // show building (since that where queued will transition to).
    //
    if (bss.queued_count != 0 || bss.building_count != 0)
    {
      if (bss.queued_count != 0)
        os << bss.queued_count << " queued, ";

      os << bss.building_count << " building, ";
    }

    os << bss.failure_count << " failed";
    if (!warning_success && bss.warning_count != 0)
      os << " (" << bss.warning_count << " due to warnings)";

    os << ", " << bss.success_count << " succeeded";
    if (warning_success && bss.warning_count != 0)
      os << " (" << bss.warning_count << " with warnings)";

    // Note that the warning count has already been included in the success or
    // failure count (see calc_build_stats() for details).
    //
    size_t total (bss.queued_count + bss.building_count +
                  bss.success_count + bss.failure_count);
    os << ", " << total << " total";

    return os.str ();
  }

  uint64_t ci_github::
  report_budget (const gq_rate_limits& limits, const basic_mark& error) const
  {
    assert (limits.reset != timestamp_unknown);

    // Let's reserve 10% of the total budget for the cases when the actual
    // number of CI jobs exceeds the configured expected maximum for some time
    // frame. This way, at least the aggregate reporting mode (without any
    // statistics updates) will be available for the excessive jobs.
    //
    uint64_t reserve (limits.limit / 10);
    uint64_t remaining (limits.remaining);

    if (remaining <= reserve)
      return 0;

    remaining -= reserve;

    // Return the whole remaining budget, if configured to do so.
    //
    uint64_t max_jobs (options_->ci_github_max_jobs_per_window ());

    if (max_jobs == 0)
      return remaining;

    // Calculate the job budget, but bail out if something feels off.
    //
    uint64_t window_size (3600); // 1 hour.

    uint64_t reset (
      chrono::duration_cast<chrono::seconds> (
        limits.reset.time_since_epoch ()).count ());

    uint64_t now (
      chrono::duration_cast<chrono::seconds> (
        system_clock::now ().time_since_epoch ()).count ());

    // If the current time is equal or insignificantly greater (say by 60
    // seconds) than the reset time point, then assume that the new rate limit
    // window just started and the total budget is available again. If it is
    // greater significantly, then something is probably off, so just report
    // and bail out.
    //
    if (now >= reset)
    {
      if (now - reset > 60)
      {
        error << "rate limit reset time point is " << now - reset
              << " seconds ago";

        return 0;
      }
      else
      {
        reset += window_size;
        remaining = limits.limit - reserve;
      }
    }

    // If the time left until the reset time point is greater then the window
    // size, then we probably assume the wrong window size. Let's report and
    // bail out in this case.
    //
    uint64_t left (reset - now); // Seconds left until the reset time point.

    if (left > window_size)
    {
      error << "current rate limit window is greater than " << window_size
            << " seconds: " << left << " seconds left until reset";

      return 0;
    }

    // Approximate the number of jobs remaining until the reset time point (as
    // jobs = max_jobs * left / window_size), rounding to the closest integer.
    // Also assume there is at least 1 job ahead.
    //
    uint64_t jobs (max ((max_jobs * left + (window_size / 2)) / window_size,
                        uint64_t (1)));

    return remaining / jobs;
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
  //    conclusion (which would be pretty confusing for the user). Note that
  //    the ->queued state transition of a check run rebuild triggered by
  //    us is handled directly in handle_check_run_rerequest().
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
  function<optional<string> (const string&, const tenant_service&)> ci_github::
  build_queued (const string& tenant_id,
                const tenant_service& ts,
                const vector<build>& builds,
                optional<build_state> istate,
                const build_queued_hints& hs,
                const diag_epilogue& log_writer) const noexcept
  try
  {
    // NOTE: this function is noexcept and should not throw.

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

    // Ignore attempts to add new builds to a completed check suite. This can
    // happen, for example, if a new build configuration is added before
    // the tenant is archived.
    //
    if (sd.completed)
      return nullptr;

    // The builds for which we will be creating check runs.
    //
    vector<reference_wrapper<const build>> bs;
    brep::check_runs crs; // Parallel to bs.

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
          // Note that handle_check_run_rerequest() may trigger an "expected"
          // rebuild, in which case our state should be set to queued.
          //
          if (scr->state != build_state::queued || !scr->state_synced)
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

        crs.push_back (
          check_run {move (bid),
                     gh_check_run_name (b, &hs),
                     nullopt, /* node_id */
                     build_state::queued,
                     false /* state_synced */,
                     nullopt /* status */,
                     details_url (b),
                     check_run::description_type {check_run_queued_title,
                                                  check_run_queued_summary}});
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
      if (optional<string> jwt = generate_jwt (sd.app_id, trace, error))
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

    // Determine the reporting mode: detailed or aggregate.
    //
    // In the aggregate reporting mode we don't actually update the check runs
    // on GitHub; we only simulate it by updating the local check run objects
    // in the same way a GitHub update would have.
    //
    // Note: don't go into the aggregate reporting mode if we were already in
    // the detailed reporting mode (which could occur if the check suite was
    // re-requested). Going from detailed to the aggregate reporting mode
    // would cause the existing build check runs to be left in an outdated
    // state indefinitely.

    // Reporting mode is only determined and saved in this function so it must
    // be undetermined in the service data unless this is a re-request.
    //
    assert (sd.report_mode == report_mode::undetermined || sd.re_request);

    report_mode rm (report_mode::undetermined);

    uint64_t builds_limit (options_->ci_github_builds_aggregate_report ());

    // For each build in the detailed mode assume 1 point for reporting
    // transition into the building state plus 1 point -- into the built
    // state. For simplicity, we don't take into account some other
    // notifications sent once per CI job (queued, etc), since this is all
    // very approximate anyway.
    //
    bool aggregate ((builds_limit != 0 && crs.size () > builds_limit) ||
                    crs.size () * 2 > sd.report_budget);

    switch (sd.report_mode)
    {
    case report_mode::undetermined:
    case report_mode::aggregate:
      {
        rm = aggregate ? report_mode::aggregate : report_mode::detailed;
        break;
      }
    case report_mode::detailed:
      {
        // Never switch out of the detailed mode.

        if (aggregate)
        {
          warn << "not switching from detailed to aggregate reporting mode, "
               << "budget: " << sd.report_budget << ", builds: "
               << crs.size ();
        }

        rm = report_mode::detailed;
        break;
      }
    }

    // Note: we treat the failure to obtain the installation access token the
    // same as the failure to notify GitHub (state is updated by not marked
    // synced).
    //
    if (iat != nullptr)
    {
      switch (rm)
      {
      case report_mode::detailed:
        {
          // Create a check_run for each build as a single request.
          //
          // Let unlikely invalid_argument propagate.
          //
          gq_rate_limits limits;
          if (gq_create_check_runs (error,
                                    crs,
                                    iat->token,
                                    sd.app_id,
                                    sd.repository_node_id,
                                    sd.report_sha,
                                    options_->build_queued_batch (),
                                    &limits))
          {
            for (const check_run& cr: crs)
            {
              // We can only create a check run in the queued state.
              //
              assert (cr.state == build_state::queued);
              l3 ([&]{trace << "created check_run { " << cr << " }";});
            }
          }

          info << "installation id " << sd.installation_id << " limits: "
               << limits;

          break;
        }
      case report_mode::aggregate:
        {
          // Don't actually update the check runs on GitHub; only simulate the
          // updates and save the check runs (but note that the node ids will
          // remain absent).
          //
          for (check_run& cr: crs)
          {
            assert (cr.state == build_state::queued); // Set above.
            cr.state_synced = true;
          }

          // Update the conclusion check run with build stats (it may be a
          // while until we get the first build_built() notification).
          {
            assert (sd.conclusion_node_id.has_value ());

            check_run ccr;
            ccr.name = conclusion_check_run_name (sd.app_id);
            ccr.state_synced = false;

            string r; // Build stats report.
            {
              build_stats s (calculate_build_stats (crs, sd.warning_success));

              // The queued notification is delivered when the first build bot
              // picks up a job so factor the imminent queued->building
              // transition into the build stats.
              //
              --s.queued_count;
              ++s.building_count;

              r = make_build_stats_report (s, sd.warning_success);
            }

            if (gq_update_check_run (error,
                                     ccr,
                                     iat->token,
                                     sd.repository_node_id,
                                     *sd.conclusion_node_id,
                                     build_state::building,
                                     conclusion_building_title,
                                     r + ". " + force_rebuild_md_link (sd) +
                                     '.'))
            {
              assert (ccr.state == build_state::building);
              l3 ([&]{trace << "updated conclusion check_run { " << ccr << " }";});
            }
          }

          break;
        }
      case report_mode::undetermined: assert (false);
      }
    }

    return [tenant_id,
            bs = move (bs),
            iat = move (new_iat),
            crs = move (crs),
            rm,
            error = move (error),
            warn = move (warn)] (const string& ti,
                                 const tenant_service& ts) -> optional<string>
    {
      // NOTE: this lambda may be called repeatedly (e.g., due to transaction
      // being aborted) and so should not move out of its captures.

      if (tenant_id != ti)
        return nullopt; // Do nothing if the tenant has been replaced.

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

      sd.report_mode = rm;

      return sd.json ();
    };
  }
  catch (const std::exception& e)
  {
    NOTIFICATION_DIAG (log_writer);

    error << "CI tenant " << ts.id << ": unhandled exception: " << e.what ();

    return nullptr;
  }

  function<optional<string> (const string&, const tenant_service&)> ci_github::
  build_building (const string& tenant_id,
                  const tenant_service& ts,
                  const build& b,
                  const diag_epilogue& log_writer) const noexcept
  try
  {
    // NOTE: this function is noexcept and should not throw.

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

    // Similar to build_queued(), ignore attempts to add new builds to a
    // completed check suite.
    //
    if (sd.completed)
      return nullptr;

    // In addition to updating the build check run we also update the
    // conclusion check run with the build stats. If we're in the aggregate
    // reporting mode on the other hand no check runs are updated on GitHub
    // but the local build check run object is updated to simulate a GitHub
    // update.

    // The build and conclusion check run updates are sent to GitHub in a
    // single request so store them together from the outset.
    //
    brep::check_runs check_runs (2);
    check_run& bcr (check_runs[0]); // Build check run.
    check_run& ccr (check_runs[1]); // Conclusion check run.

    // Reflect the current state of the conclusion check run.
    //
    ccr.name = conclusion_check_run_name (sd.app_id);
    ccr.node_id = sd.conclusion_node_id;
    ccr.state = build_state::building;

    build_stats bstats; // Build stats for the conclusion check run.

    string bid (gh_check_run_name (b)); // Full build id.

    if (check_run* scr = sd.find_check_run (bid)) // Stored check run.
    {
      // Update the check run if it exists on GitHub and the queued
      // notification updated the service data, otherwise do nothing.
      //
      if (scr->state == build_state::queued)
      {
        switch (sd.report_mode)
        {
        case report_mode::detailed:
          {
            if (scr->node_id)
            {
              // Calculate the build stats (for the conclusion check run)
              // before moving from the stored check run.
              //
              scr->state = build_state::building; // For the calculation.
              bstats = calculate_build_stats (sd.check_runs,
                                              sd.warning_success);

              bcr = move (*scr);
            }
            else
            {
              // Network error during queued notification (state
              // unsynchronized), ignore.
              //
              l3 ([&]{trace << "unsynchronized check run " << bid;});
            }

            break;
          }
        case report_mode::aggregate:
          {
            // Won't be updating GitHub but we will be saving the check run in
            // the service data.
            //
            assert (!scr->node_id.has_value ());
            scr->state = build_state::building; // As detailed reporting case.

            bcr = move (*scr);

            break;
          }
          // Note: reporting mode cannot be undetermined if check run is
          // queued.
          //
        case report_mode::undetermined: assert (false);
        }
      }
      else
      {
        // Ignore interrupted (building -> queued -> building transition).
        //
        if (scr->state != build_state::building)
        {
          warn << "check run " << bid << ": out of order building "
               << "notification; existing state: " << scr->state_string ();
        }
      }
    }
    else
      warn << "check run " << bid << ": out of order building "
           << "notification; no check run state in service data";

    if (bcr.build_id.empty ())
      return nullptr; // Not in service data, state unsynced, or out of order.

    // If we're proceeding then the reporting mode cannot be undetermined.
    //
    assert (sd.report_mode != report_mode::undetermined);

    // Get a new installation access token if the current one has expired.
    //
    const gh_installation_access_token* iat (nullptr);
    optional<gh_installation_access_token> new_iat;

    if (system_clock::now () > sd.installation_access.expires_at)
    {
      if (optional<string> jwt = generate_jwt (sd.app_id, trace, error))
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
      switch (sd.report_mode)
      {
      case report_mode::detailed:
        {
          // Update the build and conclusion check runs.
          //
          assert (bcr.state == build_state::building); // Set above.
          bcr.state_synced = false;
          bcr.description = {check_run_building_title,
                             check_run_building_summary};

          assert (ccr.state == build_state::building);
          ccr.state_synced = false;
          {
            string r (make_build_stats_report (bstats, sd.warning_success));
            ccr.description = {conclusion_building_title,
              r + ". " + force_rebuild_md_link (sd) + '.'};
          }

          // Let unlikely invalid_argument propagate.
          //
          if (gq_update_check_runs (error,
                                    check_runs,
                                    iat->token,
                                    sd.repository_node_id))
          {
            // Do nothing further if the state was already built on GitHub
            // (note that this is based on the above-mentioned special GitHub
            // semantics of preventing changes to the built status).
            //
            if (bcr.state == build_state::built)
            {
              warn << "check run " << bid
                   << ": already in built state on GitHub";
              return nullptr;
            }

            assert (bcr.state == build_state::building);

            l3 ([&]{trace << "updated check_run { " << bcr << " }";});
            l3 ([&]{trace << "updated conclusion check_run { " << ccr << " }";});
          }
          break;
        }
      case report_mode::aggregate:
        {
          // Only simulate the GitHub update of the build check run.
          //
          // Note that in this mode we (periodically) update the conclusion
          // check runs with stats in build_built() (see there for rationale).
          //
          assert (bcr.state == build_state::building); // Set above.
          bcr.state_synced = true;

          break;
        }
        // Note that we only get here if the check run is in the queued state
        // and that means the reporting mode should have been determined.
        //
      case report_mode::undetermined: assert (false);
      }
    }

    return [tenant_id,
            iat = move (new_iat),
            cr = move (bcr),
            error = move (error),
            warn = move (warn)] (const string& ti,
                                 const tenant_service& ts) -> optional<string>
    {
      // NOTE: this lambda may be called repeatedly (e.g., due to transaction
      // being aborted) and so should not move out of its captures.

      if (tenant_id != ti)
        return nullopt; // Do nothing if the tenant has been replaced.

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
  catch (const std::exception& e)
  {
    NOTIFICATION_DIAG (log_writer);

    string bid (gh_check_run_name (b)); // Full build id.

    error << "check run " << bid << ": unhandled exception: " << e.what();

    return nullptr;
  }

  function<pair<optional<string>, bool> (const string&,
                                         const tenant_service&)> ci_github::
  build_built (const string& tenant_id,
               const tenant_service& ts,
               const build& b,
               const diag_epilogue& log_writer) const noexcept
  try
  {
    // NOTE: this function is noexcept and should not throw.

    NOTIFICATION_DIAG (log_writer);

    // @@ TODO Include ts.id in diagnostics? Check run build ids alone seem
    //    kind of meaningless. Log lines get pretty long this way however.

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

    // Similar to build_queued(), ignore attempts to add new builds to a
    // completed check suite.
    //
    if (sd.completed)
      return nullptr;

    // If we don't have the accurate list of check runs in the service data
    // (for example, because we ran out of transaction retries trying to
    // update it), then things are going to fall apart: we will add this check
    // run and then immediately conclude that the check suite is complete
    // (while GitHub will likely continue showing a bunch of queued check
    // runs. If this checks run is successful, then we will conclude the
    // check suite is successful and update the conclusion check run, all
    // based on one build.
    //
    if (sd.check_runs.empty ())
    {
      error << "no queued check runs in service data for tenant " << tenant_id;
      return nullptr;
    }

    // Here we only update the state of this check run. If there are no more
    // unbuilt ones, then the synthetic conclusion check run will be updated
    // in build_completed(). Note that determining whether we have no more
    // unbuilt would be racy here so instead we do it in the service data
    // update function that we return.
    //
    // In the aggregated reporting mode we update the conclusion check run on
    // GitHub (with the latest build stats) and only simulate the GitHub
    // update of the build check run (just as we do in build_queued() and
    // build_building()).
    //
    // To summarize, in the detailed reporting mode we update only the build
    // check run on GitHub and in the aggregate reporting mode we update only
    // the conclusion check run on GitHub. The reason we do the latter here
    // and not in build_building() (as in the detailed mode) is to avoid
    // races: it is a lot more likely to simultaneously receive multiple
    // building notifications than built. And this could lead to multiple
    // notifications seeing the same counts and trying to update the
    // conclusion check run.
    //
    check_run cr;       // Updated check run.
    build_stats bstats; // Build stats (for the aggregate reporting mode).
    {
      string bid (gh_check_run_name (b)); // Full build id.

      if (check_run* scr = sd.find_check_run (bid))
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

        // Calculate build stats for the conclusion check run if in the
        // aggregate reporting mode.
        //
        // Note that we treat the undetermined reporting mode the same as the
        // detailed mode (see below for details).
        //
        if (sd.report_mode == report_mode::aggregate)
        {
          scr->state = build_state::built; // Required for the calculation.
          scr->status = b.status;          // Required for the calculation.
          bstats = calculate_build_stats (sd.check_runs, sd.warning_success);
        }

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

        // Calculate build stats for the conclusion check run if in aggregate
        // reporting mode.
        //
        if (sd.report_mode == report_mode::aggregate)
        {
          cr.state = build_state::built; // Required for the calculation.
          cr.status = b.status;          // Required for the calculation.
          sd.check_runs.push_back (move (cr));
          bstats = calculate_build_stats (sd.check_runs, sd.warning_success);
          cr = move (sd.check_runs.back ());
        }
      }

      cr.state_synced = false;
    }

    // Get a new installation access token if the current one has expired.
    //
    const gh_installation_access_token* iat (nullptr);
    optional<gh_installation_access_token> new_iat;

    if (system_clock::now () > sd.installation_access.expires_at)
    {
      if (optional<string> jwt = generate_jwt (sd.app_id, trace, error))
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
      switch (sd.report_mode)
      {
      case report_mode::detailed:
        {
          // Prepare the check run's summary field (the build information in
          // an XHTML table).
          //
          string sm; // Summary.
          {
            using namespace web::xhtml;

            // Note: let all serialization exceptions propagate. The XML
            // serialization code can throw bad_alloc or xml::serialization in
            // case of I/O failures, but we're serializing to a string stream
            // so both exceptions are unlikely.
            //
            ostringstream os;
            xml::serializer s (os, "check_run_summary");

            // This hack is required to disable XML element name prefixes
            // (which GitHub does not like). Note that this adds an xmlns
            // declaration for the XHTML namespace which for now GitHub
            // appears to ignore. If that ever becomes a problem, then we
            // should redo this with raw XML serializer calls.
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

            // Serialize a result row (colored circle, result text, log URL)
            // for an operation and result_status.
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
            make_built_result (*b.status, sd.warning_success, move (sm)));

          if (cr.node_id)
          {
            // Update existing check run to built. Let unlikely
            // invalid_argument propagate.
            //
            if (gq_update_check_run (error,
                                     cr,
                                     iat->token,
                                     sd.repository_node_id,
                                     *cr.node_id,
                                     move (br)))
            {
              assert (cr.state == build_state::built);
              l3 ([&]{trace << "updated check_run { " << cr << " }";});
            }
          }
          else
          {
            // Create new check run. Let unlikely invalid_argument propagate.
            //
            // Note that we don't have build hints so will be creating this
            // check run with the full build id as name. In the unlikely event
            // that an out of order build_queued() were to run before we've
            // saved this check run to the service data it will create another
            // check run with the shortened name which will never get to the
            // built state.
            //
            if (gq_create_check_run (error,
                                     cr,
                                     iat->token,
                                     sd.app_id,
                                     sd.repository_node_id,
                                     sd.report_sha,
                                     details_url (b),
                                     move (br)))
            {
              assert (cr.state == build_state::built);
              l3 ([&]{trace << "created check_run { " << cr << " }";});
            }
          }

          break;
        }
      case report_mode::aggregate:
        {
          // Update the conclusion check run on GitHub with the current build
          // stats if this build falls on a report interval (i.e., the current
          // completed count is a multiple of the interval, the size of which
          // is calculated to keep us within our report budget).

          // Note that the current build has already been counted as built.
          //
          size_t built_count (bstats.success_count + bstats.failure_count);

          // If the report budget is greater than or equal to the number of
          // builds, report on every build (interval value 1).
          //
          size_t total_count (bstats.queued_count + bstats.building_count +
                              bstats.success_count + bstats.failure_count);

          size_t report_interval (sd.report_budget < total_count
                                  ? total_count / sd.report_budget
                                  : 1);

          if (built_count % report_interval == 0)
          {
            assert (sd.conclusion_node_id.has_value ());

            check_run ccr;
            ccr.name = conclusion_check_run_name (sd.app_id);
            ccr.state_synced = false;

            string r (make_build_stats_report (bstats, sd.warning_success));

            if (gq_update_check_run (error,
                                     ccr,
                                     iat->token,
                                     sd.repository_node_id,
                                     *sd.conclusion_node_id,
                                     build_state::building,
                                     conclusion_building_title,
                                     r + ". " + force_rebuild_md_link (sd) +
                                       '.'))
            {
              assert (ccr.state == build_state::building);
              l3 ([&]{trace << "updated conclusion check_run { " << ccr << " }";});
            }
          }

          // Simulate the update of the build check run on GitHub.
          //
          assert (cr.state == build_state::built); // Set above.
          assert (cr.status.has_value ());         // Set above.
          cr.state_synced = true;

          break;
        }
      case report_mode::undetermined:
        {
          // Reporting mode could theoretically be undetermined if this is an
          // out-of-order notification so let's not assert.

          string bid (gh_check_run_name (b)); // Full build id.

          error << "check run " << bid << ": reporting mode is undetermined";

          return nullptr;
        }
      }

      // Ensure we only save a result_status if the build_state has been
      // synced with GitHub.
      //
      assert (cr.state_synced || !cr.status.has_value ());

      if (cr.state_synced)
      {
        // Check run was created/updated successfully to built (with status we
        // specified).
        //
        cr.status = b.status;
      }
    }

    return [tenant_id,
            iat = move (new_iat),
            cr = move (cr),
            error = move (error),
            warn = move (warn)] (const string& ti,
                                 const tenant_service& ts)
    {
      // NOTE: this lambda may be called repeatedly (e.g., due to transaction
      // being aborted) and so should not move out of its captures.

      // Do nothing if the tenant has been replaced.
      //
      if (tenant_id != ti)
        return make_pair (optional<string> (), false);

      service_data sd;
      try
      {
        sd = service_data (*ts.data);
      }
      catch (const invalid_argument& e)
      {
        error << "failed to parse service data: " << e;
        return make_pair (optional<string> (), false);
      }

      // Feel like this could potentially happen in case of an out of order
      // notification (see above).
      //
      if (sd.completed)
      {
        // @@ Perhaps this should be a warning but let's try error for now (we
        //    essentially missed a build, which could have failed).
        //
        error << "built notification for completed check suite";
        return make_pair (optional<string> (), false);
      }

      if (iat)
        sd.installation_access = *iat;

      // Only update the check_run state in service data if it matches the
      // state (specifically, status) on GitHub.
      //
      if (cr.state_synced)
      {
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
          *scr = cr; // Note: also updates node id if created.
        }
        else
          sd.check_runs.push_back (cr);

        // Determine of this check suite is completed.
        //
        sd.completed = find_if (sd.check_runs.begin (), sd.check_runs.end (),
                                [] (const check_run& scr)
                                {
                                  return scr.state != build_state::built;
                                }) == sd.check_runs.end ();
      }

      return make_pair (optional<string> (sd.json ()), sd.completed);
    };
  }
  catch (const std::exception& e)
  {
    NOTIFICATION_DIAG (log_writer);

    string bid (gh_check_run_name (b)); // Full build id.

    error << "check run " << bid << ": unhandled exception: " << e.what ();

    return nullptr;
  }

  void ci_github::
  build_completed (const string& /* tenant_id */,
                   const tenant_service& ts,
                   const diag_epilogue& log_writer) const noexcept
  try
  {
    // NOTE: this function is noexcept and should not throw.

    NOTIFICATION_DIAG (log_writer);

    service_data sd;
    try
    {
      sd = service_data (*ts.data);
    }
    catch (const invalid_argument& e)
    {
      error << "failed to parse service data: " << e;
      return;
    }

    // This could have been reset by handle_check_run_rerequest().
    //
    if (!sd.completed)
      return;

    assert (!sd.check_runs.empty ());

    // Here we need to update the state of the synthetic conclusion check run.

    // Build states count breakdown and aggregated result status for the
    // builds.
    //
    build_stats bss (calculate_build_stats (sd.check_runs, sd.warning_success));

    assert (bss.result.has_value ()); // We know the builds are all complete.

    // Conclusion check run summary. Append the force rebuild link.
    //
    string summary (make_build_stats_report (bss, sd.warning_success) +
                    ". " + force_rebuild_md_link (sd) + '.');

    // Get a new installation access token if the current one has expired
    // (unlikely since we just returned from build_built()). Note also that we
    // are not saving the new token in the service data.
    //
    const gh_installation_access_token* iat (nullptr);
    optional<gh_installation_access_token> new_iat;

    if (system_clock::now () > sd.installation_access.expires_at)
    {
      if (optional<string> jwt = generate_jwt (sd.app_id, trace, error))
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
    // same as the failure to notify GitHub.
    //
    if (iat != nullptr)
    {
      // Update the conclusion check run if all check runs are now built.
      //
      assert (sd.conclusion_node_id);

      gq_built_result br (
        make_built_result (*bss.result, sd.warning_success, move (summary)));

      check_run cr;

      // Set some fields for display purposes.
      //
      cr.node_id = *sd.conclusion_node_id;
      // Let invalid_argument propagate.
      //
      cr.name = conclusion_check_run_name (sd.app_id);

      // Let unlikely invalid_argument propagate.
      //
      gq_rate_limits limits;
      if (gq_update_check_run (error,
                               cr,
                               iat->token,
                               sd.repository_node_id,
                               *sd.conclusion_node_id,
                               move (br),
                               &limits))
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

      info << "installation id " << sd.installation_id << " limits: "
           << limits;
    }
  }
  catch (const std::exception& e)
  {
    NOTIFICATION_DIAG (log_writer);

    error << "unhandled exception: " << e.what ();
  }

  void ci_github::
  build_canceled (const string& /* tenant_id */,
                  const tenant_service& ts,
                  const diag_epilogue& log_writer) const noexcept
  try
  {
    // NOTE: this function is noexcept and should not throw.

    NOTIFICATION_DIAG (log_writer);

    // We end up here when the service data could not be saved (for example,
    // due to persistent transaction conflicts, which does happen if the user
    // requests a rebuild of a large number of failed check runs).
    //
    // Note that we cannot recover from this situation since now our state (in
    // service data) does not match the state on GitHub. Ideally in this case
    // we would like to fail the conclusion check run and ask the user to
    // re-request the entire check suite. However, failing the conclusion is
    // not enough -- we also need to either remove all other check runs or to
    // at least change them to the completed state (failed that, GitHub UI
    // won't allow the user to re-request the check suite). Unfortunately,
    // there is no way to remove check runs on GitHub nor to change the state
    // of all the check runs that match a certain criteria. The only way is
    // to specify each check run mutation with its node id (which we may not
    // have). So the only way to implement this would be to query all the
    // existing check runs (with pagination and all), and then change them to
    // the completed state (again, probably in batches).
    //
    // So instead of going through all this trouble, we are going to just
    // re-request the check suite ourselves. Luckily the GitHub API allows
    // this even if the check suite is not completed. This is not ideal since
    // we may cause an infinite failure cycle, but seem to be the best we can
    // do without heroic measures.
    //
    // Note also that the tenant still contains the original service data
    // and which we need in certain cases in handle_check_suite_rerequest().

    // Parse the unsaved service data.
    //
    service_data sd;
    try
    {
      sd = service_data (*ts.data);
    }
    catch (const invalid_argument& e)
    {
      error << "failed to parse service data: " << e;
      return;
    }

    // Get a new installation access token if the current one has expired.
    //
    const gh_installation_access_token* iat (nullptr);
    optional<gh_installation_access_token> new_iat;

    if (system_clock::now () > sd.installation_access.expires_at)
    {
      if (optional<string> jwt = generate_jwt (sd.app_id, trace, error))
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

    if (iat != nullptr)
    {
      // Re-request the check suite.
      //
      // Note that the conclusion check run is created before the tenant is
      // loaded so the unsaved service data should normally contain the check
      // suite node id, but let's not assume, just in case.
      //
      if (sd.check_suite_node_id)
      {
        const string& nid (*sd.check_suite_node_id);

        // Let unlikely invalid_argument propagate.
        //
        if (gq_rerequest_check_suite (error,
                                      iat->token,
                                      sd.repository_node_id,
                                      nid))
        {
          l3 ([&]{trace << "re-requested check suite " << nid;});
        }
        else
          error << "failed to re-request check suite " << nid;
      }
    }
  }
  catch (const std::exception& e)
  {
    NOTIFICATION_DIAG (log_writer);

    error << "unhandled exception: " << e.what ();
  }

  string ci_github::
  details_url (const build& b) const
  {
    // This code is based on build_force_url() in mod/build.cxx.
    //
    return
      options_->host ()                                               +
      tenant_dir (options_->root (), b.tenant).string ()              +
      "?builds=" + mime_url_encode (b.package_name.string ())         +
      "&pv=" + mime_url_encode (b.package_version.string ())          +
      "&tg=" + mime_url_encode (b.target.string ())                   +
      "&tc=" + mime_url_encode (b.target_config_name)                 +
      "&pc=" + mime_url_encode (b.package_config_name)                +
      "&th=" + mime_url_encode (b.toolchain_name)                     + '-' +
                                b.toolchain_version.string ();
  }

  string ci_github::
  details_url (const string& t) const
  {
    return
      options_->host () +
      tenant_dir (options_->root (), t).string () +
      "?builds";
  }

  string ci_github::
  force_rebuild_md_link (const service_data& sd) const
  {
    return
      "[Force rebuild](" +
      options_->host () +
      options_->root ().string () +
      "?ci-github=rerequest" +
      "&repo-id=" + sd.repository_node_id +
      "&head-sha=" + sd.report_sha +
      "&reason=)";
  }

  static optional<build_id>
  parse_details_url (const string& details_url)
  try
  {
    // See details_url() above for an idea of what the URL looks like.

    url u (details_url);

    build_id r;

    // Extract the tenant from the URL path.
    //
    // Example paths:
    //
    //   @d2586f57-21dc-40b7-beb2-6517ad7917dd (37 characters)
    //   <brep-root>/@d2586f57-21dc-40b7-beb2-6517ad7917dd
    //
    if (!u.path)
      return nullopt;

    {
      size_t p (u.path->find ('@'));
      if (p == string::npos || u.path->size () - p != 37)
        return nullopt; // Tenant not found or too short.

      r.package.tenant = u.path->substr (p + 1);
    }

    // Extract the rest of the build_id members from the URL query.
    //
    if (!u.query)
      return nullopt;

    bool pn (false), pv (false), tg (false), tc (false), pc (false),
      th (false);

    // This URL query parsing code is based on
    // web::apache::request::parse_url_parameters().
    //
    for (const char* qp (u.query->c_str ()); qp != nullptr; )
    {
      const char* vp (strchr (qp, '='));
      const char* ep (strchr (qp, '&'));

      if (vp == nullptr || (ep != nullptr && ep < vp))
        return nullopt; // Missing value.

      string n (mime_url_decode (qp, vp)); // Name.

      ++vp; // Skip '='

      const char* ve (ep != nullptr ? ep : vp + strlen (vp)); // Value end.

      // Get the value as-is or URL-decode it.
      //
      auto rawval = [vp, ve] () { return string (vp, ve); };
      auto decval = [vp, ve] () { return mime_url_decode (vp, ve); };

      auto make_version = [] (string&& v)
      {
        return canonical_version (brep::version (move (v)));
      };

      auto c = [&n] (bool& b, const char* s)
      {
        return n == s ? (b = true) : false;
      };

      if (c (pn, "builds"))  r.package.name        = package_name (decval ());
      else if (c (pv, "pv")) r.package.version     = make_version (decval ());
      else if (c (tg, "tg")) r.target              = target_triplet (decval ());
      else if (c (tc, "tc")) r.target_config_name  = decval ();
      else if (c (pc, "pc")) r.package_config_name = decval ();
      else if (c (th, "th"))
      {
        // Toolchain name and version. E.g. "public-0.17.0"

        string v (rawval ());

        // Note: parsing code based on mod/mod-builds.cxx.
        //
        size_t p (v.find ('-'));
        if (p == string::npos || p >= v.size () - 1)
          return nullopt; // Invalid format.

        r.toolchain_name    = v.substr (0, p);
        r.toolchain_version = make_version (v.substr (p + 1));
      }

      qp = ep != nullptr ? ep + 1 : nullptr;
    }

    if (!pn || !pv || !tg || !tc || !pc || !th)
      return nullopt; // Fail if any query parameters are absent.

    return r;
  }
  catch (const invalid_argument&) // Invalid url, brep::version, etc.
  {
    return nullopt;
  }

  optional<string> ci_github::
  generate_jwt (uint64_t app_id,
                const basic_mark& trace,
                const basic_mark& error) const
  {
    string jwt;
    try
    {
      // Look up the private key path for the app id and fail if not found.
      //
      const map<uint64_t, dir_path>& pks (
        options_->ci_github_app_id_private_key ());

      auto pk (pks.find (app_id));
      if (pk == pks.end ())
      {
        error << "unable to generate JWT: "
              << "no private key configured for app id " << app_id;
        return nullopt;
      }

      // Set token's "issued at" time 60 seconds in the past to combat clock
      // drift (as recommended by GitHub).
      //
      jwt = brep::generate_jwt (
          *options_,
          pk->second, to_string (app_id),
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
  obtain_installation_access_token (const string& iid,
                                    string jwt,
                                    const basic_mark& error) const
  {
    gh_installation_access_token iat;
    try
    {
      // API endpoint.
      //
      string ep ("app/installations/" + iid + "/access_tokens");

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
    // gh_installation_access_token (via github_post())
    //
    catch (const json::invalid_json_input& e)
    {
      // Note: e.name is the GitHub API endpoint.
      //
      error << "malformed JSON in response from " << e.name << ", line: "
            << e.line << ", column: " << e.column << ", byte offset: "
            << e.position << ", error: " << e;
      return nullopt;
    }
    catch (const invalid_argument& e) // github_post()
    {
      error << "malformed header(s) in response: " << e;
      return nullopt;
    }
    catch (const system_error& e) // github_post()
    {
      error << "unable to get installation access token (errno=" << e.code ()
            << "): " << e.what ();
      return nullopt;
    }

    return iat;
  }
}
