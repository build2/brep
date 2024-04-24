// file      : mod/mod-ci-github.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-ci-github.hxx>

#include <libbutl/json/parser.hxx>

#include <mod/jwt.hxx>
#include <mod/hmac.hxx>
#include <mod/module-options.hxx>

#include <mod/mod-ci-github-gq.hxx>
#include <mod/mod-ci-github-post.hxx>
#include <mod/mod-ci-github-service-data.hxx>

#include <stdexcept>

// @@ TODO
//
//    Building CI checks with a GitHub App
//    https://docs.github.com/en/apps/creating-github-apps/writing-code-for-a-github-app/building-ci-checks-with-a-github-app
//

// @@ TODO Best practices
//
//    Webhooks:
//    https://docs.github.com/en/webhooks/using-webhooks/best-practices-for-using-webhooks
//    https://docs.github.com/en/webhooks/using-webhooks/validating-webhook-deliveries
//
//    REST API:
//    https://docs.github.com/en/rest/using-the-rest-api/best-practices-for-using-the-rest-api?apiVersion=2022-11-28
//
//    Creating an App:
//    https://docs.github.com/en/apps/creating-github-apps/about-creating-github-apps/best-practices-for-creating-a-github-app
//
//    Use a webhook secret to ensure request is coming from Github. HMAC:
//    https://en.wikipedia.org/wiki/HMAC#Definition. A suitable implementation
//    is provided by OpenSSL.

// @@ TODO Centralize exception/error handling around calls to
//         github_post(). Currently it's mostly duplicated and there is quite
//         a lot of it.
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
      : handler (r),
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
    if (options_->ci_github_app_webhook_secret_specified ())
    {
      ci_start::init (make_shared<options::ci_start> (*options_));
    }
  }

  bool ci_github::
  handle (request& rq, response&)
  {
    using namespace bpkg;

    HANDLER_DIAG;

    if (!options_->ci_github_app_webhook_secret_specified ())
      throw invalid_request (404, "GitHub CI request submission disabled");

    // Process headers.
    //
    // @@ TMP Shouldn't we also error<< in some of these header problem cases?
    //
    // @@ TMP From GitHub docs: "You can create webhooks that subscribe to the
    //        events listed on this page."
    //
    //        So it seems appropriate to generally use the term "event" (which
    //        we already do for the most part), and "webhook event" only when
    //        more context would be useful?
    //
    string event; // Webhook event.
    string hmac;  // Received HMAC.
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

    // Read the entire request body into a buffer because we need to compute
    // an HMAC over it and then parse it as JSON. The alternative of reading
    // from the stream twice works out to be more complicated (see also @@
    // TODO item in web/server/module.hxx).
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

    // There is a webhook event (specified in the x-github-event header) and
    // each event contains a bunch of actions (specified in the JSON request
    // body).
    //
    // Note: "GitHub continues to add new event types and new actions to
    // existing event types." As a result we ignore known actions that we are
    // not interested in and log and ignore unknown actions. The thinking here
    // is that we want be "notified" of new actions at which point we can decide
    // whether to ignore them or to handle.
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
        return handle_check_suite_request (move (cs));
      }
      else if (cs.action == "rerequested")
      {
        // Someone manually requested to re-run the check runs in this check
        // suite. Treat as a new request.
        //
        return handle_check_suite_request (move (cs));
      }
      else if (cs.action == "completed")
      {
        // GitHub thinks that "all the check runs in this check suite have
        // completed and a conclusion is available". Looks like this one we
        // ignore?
        //
        // @@ TODO What if our bookkeeping says otherwise? See conclusion
        //    field which includes timedout. Need to come back to this once
        //    have the "happy path" implemented.
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
      // @@ TODO

      throw invalid_request (501, "pull request events not implemented yet");
    }
    else
    {
      // Log to investigate.
      //
      error << "unexpected event '" << event << "'";

      throw invalid_request (400, "unexpected event: '" + event + "'");
    }
  }

  bool ci_github::
  handle_check_suite_request (gh_check_suite_event cs)
  {
    HANDLER_DIAG;

    l3 ([&]{trace << "check_suite event { " << cs << " }";});

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

    // Submit the CI request.
    //
    repository_location rl (cs.repository.clone_url + '#' +
                                cs.check_suite.head_branch,
                            repository_type::git);

    string sd (service_data (move (iat->token),
                             iat->expires_at,
                             cs.installation.id,
                             move (cs.repository.node_id),
                             move (cs.check_suite.head_sha))
                   .json ());

    optional<start_result> r (
        start (error,
               warn,
               verb_ ? &trace : nullptr,
               tenant_service (move (cs.check_suite.node_id),
                               "ci-github",
                               move (sd)),
               move (rl),
               vector<package> {},
               nullopt, // client_ip
               nullopt  // user_agent
               ));

    if (!r)
      fail << "unable to submit CI request";

    return true;
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
  //    conclusion (which would be pretty confusing for the user).
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
  //    built     Update if there is check run in service data and its
  //              state is not built, otherwise create new.
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
      string bid (gh_check_run_name (b)); // Full build ID.

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
          // Unexpectd built->queued transition (rebuild).
          //
          warn << "check run " << bid << ": unexpected rebuild";
        }
        else
          ; // Ignore interrupted.
      }
      else
      {
        // No stored check run for this build so prepare to create one.
        //
        bs.push_back (b);

        crs.emplace_back (move (bid),
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

    if (iat != nullptr)
    {
      // Create a check_run for each build.
      //
      if (gq_create_check_runs (crs,
                                iat->token,
                                sd.repository_id, sd.head_sha,
                                bs,
                                build_state::queued,
                                hs,
                                error))
      {
        for (const check_run& cr: crs)
          l3 ([&]{trace << "created check_run { " << cr << " }";});
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
  build_building (const tenant_service& ts, const build& b,
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

    check_run cr; // Updated check run.

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

    if (iat != nullptr)
    {
      string bid (gh_check_run_name (b)); // Full Build ID.

      // Stored check run.
      //
      const check_run* scr (sd.find_check_run (bid));

      if (scr != nullptr && scr->node_id)
      {
        // The check run exists on GitHub and in the persisted service data.
        //
      }
      else // (src == nullptr || !scr->node_id)
      {
        // No state has been persisted, or one or both of the other
        // notifications were unable to create the check run on GitHub.
        //
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
      }
      else
      {
      }

      return sd.json ();
    };
  }

  function<optional<string> (const tenant_service&)> ci_github::
  build_built (const tenant_service&, const build&,
               const diag_epilogue& /* log_writer */) const noexcept
  {
    return nullptr;
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
