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

// @@ TODO LATEST PROBLEMS
//
//    1) GH allows following transitions:
//
//    queued   <--> building
//    queued    --> built
//    building  --> built
//
//    I.e., GH does not allow transitions away from built. So this simplifies
//    things for us.
//
//    2) Create check run does not fail if an CR with the same name already
//       exists: instead it destroys the existing check run before creating
//       the new check run (with a new node ID). And to a GH user this appears
//       exactly like a transition from built to building/queued.
//
//    So if the first notification's data has not yet been stored, before
//    creating the CR we first need to check whether it already exists on GH
//    and then create or update as appropriate.
//
//    For build_queued() we can get the list of check runs in a check suite,
//    100 max at a time (pagination), so it can be done in N/100
//    exchanges. And using GraphQL the response would be much smaller (return
//    only the CR name).
//
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

  function<optional<string> (const tenant_service&)> ci_github::
  build_queued (const tenant_service& ts,
                const vector<build>& builds,
                optional<build_state> istate,
                const build_hints& hs,
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

    // All builds except those for which this notification is out of order and
    // thus would cause a spurious backwards state transition.
    //
    vector<reference_wrapper<const build>> bs;
    vector<check_run> crs; // Parallel to bs.

    // Exclude builds for which this is an out of order notification.
    //
    for (const build& b: builds)
    {
      // To keep things simple we are going to queue/create a new check run
      // only if we have no corresponding state (which means we haven't yet
      // done anything about this check run).
      //
      // In particular, this will ignore the building->queued (interrupted)
      // transition so on GitHub the check run will continue showing as
      // building, which is probably not a big deal. Also, this sidesteps
      // various "absent state" corner.
      //
      // Note: never go back on the built state.
      //
      string bid (gh_check_run_name (b)); // Full Build ID.

      const check_run* scr (sd.find_check_run (bid));

      if (scr == nullptr)
      {
        crs.emplace_back (move (bid), nullopt, nullopt);
        bs.push_back (b);
      }
      else if (!scr->state)
        ; // Ignore network issue.
      else if (istate && *istate == build_state::building)
        ; // Ignore interrupted.
      else
      {
        // Out of order queued notification or a rebuild (not allowed).
        //
        warn << *scr << ": "
             << "unexpected transition from "
             << (istate ? to_string (*istate) : "null") << " to "
             << to_string (build_state::queued)
             << "; previously recorded check_run state: "
             << scr->state_string ();
      }
    }

    if (bs.empty ()) // Notification is out of order for all builds.
      return nullptr;

    // What if we could not notify GitHub about some check runs due to, say, a
    // transient network? In this case we save them with the absent state
    // hoping for things to improve when we try to issue building or built
    // notifications.

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
      // @@ TODO Check whether any of these check runs exist on GH before
      //         creating them.
      //
      // Queue a check_run for each build.
      //
      if (gq_create_check_runs (crs,
                                iat->token,
                                sd.repository_id, sd.head_sha,
                                bs,
                                build_state::queued,
                                hs,
                                error))
      {
        for (check_run& cr: crs)
          l3 ([&] { trace << "created check_run { " << cr << " }"; });
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

      // Note that we've already ignored all the builds for which this
      // notification was out of order.
      //
      for (size_t i (0); i != bs.size (); ++i)
      {
        const check_run& cr (crs[i]);

        // Note that this service data may not be the same as what we observed
        // in the build_queued() function above. For example, some check runs
        // that we have queued may have already transitioned to building. So
        // we skip any check runs that are already present.
        //
        if (check_run* scr = sd.find_check_run (cr.build_id))
        {
          warn << cr << " state " << scr->state_string ()
               << " was stored before notified state " << cr.state_string ()
               << " could be stored";
        }
        else
          sd.check_runs.push_back (cr);
      }

      return sd.json ();
    };
  }

  function<optional<string> (const tenant_service&)> ci_github::
  build_building (const tenant_service& ts, const build& b,
                  const build_hints& hs,
                  const diag_epilogue& log_writer) const noexcept
  {
    // Note that we may receive this notification before the corresponding
    // check run object has been persisted in the service data (see the
    // returned by build_queued() lambda for details). Thus we wouldn't know
    // whether the check run has been created on GitHub yet or not. And given
    // that, on GitHub, creating a check run with an existent name does not
    // fail but instead replaces the existing check run (same name, different
    // node ID), we have to check whether a check run already exists on GitHub
    // before creating it.
    //
    // @@ TMP Will have to do this in build_queued() as well.
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
        if (!scr->state || *scr->state == build_state::queued)
        {
          // If the stored state is queued (most likely), at least
          // build_queued() and its lambda has run. If this is all then GitHub
          // has the queued status. But it's also possible that build_built()
          // has also run and updated GitHub to built but its lambda has not
          // yet run.
          //
          // On the other hand, if there is no stored state, both of the other
          // notifications must have run: the first created the check run on
          // GitHub and stored the node ID; the second failed to update GitHub
          // and stored the nullopt state. Either way around, build_built()
          // has run so built is the correct state.
          //
          //   @@ TMP Maybe we should always store the state and add a flag
          //          like "gh_updated", then we would know for sure whether
          //          we need to update to built or building.
          //
          // So GitHub currently either has queued or built but we can't be
          // sure which.
          //
          // If it has queued, updating to building is correct.
          //
          // If it has built then it would be a logical mistake to update to
          // building. However, GitHub ignores updates from built to any other
          // state (the REST API responds with HTTP 200 and the full check run
          // JSON body but with the "completed" status; presumably the GraphQL
          // API has the same semantics) so we can just try to update to
          // building and see what the actual status it returns is.
          //
          // If scr->state is nullopt then GitHub has either queued or built
          // but we know build_built() has run so we need to update to built
          // instead of building.
          //
          cr = move (*scr);
          cr.state = nullopt;

          build_state st (scr->state ? build_state::building
                                     : build_state::built);

          if (gq_update_check_run (cr,
                                   iat->token,
                                   sd.repository_id,
                                   *cr.node_id,
                                   st,
                                   error))
          {
            // @@ TODO If !scr->state and GH had built then we probably don't
            //         want to run the lambda either but currently it will run
            //         and this update message is not accurate. Is stored
            //         failed state the only way?
            //
            if (cr.state == st)
              l3 ([&]{trace << "updated check_run { " << cr << " }";});
            else
            {
              // Do not persist anything if state was already built on
              // GitHub.
              //
              assert (cr.state == build_state::built);

              return nullptr;
            }
          }
        }
        else if (*scr->state == build_state::built)
        {
          // Ignore out of order built notification.
          //
          assert (*scr->state == build_state::built);

          warn << *scr << ": "
               << "out of order transition from "
               << to_string (build_state::queued) << " to "
               << to_string (build_state::building) <<
            " (stored state is " << to_string (build_state::built) << ")";

          return nullptr;
        }
      }
      else // (src == nullptr || !scr->node_id)
      {
        // No state has been persisted, or one or both of the other
        // notifications were unable to create the check run on GitHub.
        //
        // It's also possible that another notification has since created the
        // check run on GitHub but its lambda just hasn't run yet.
        //
        // Thus the check run may or may not exist on GitHub.
        //
        // Because creation destroys check runs with the same name (see
        // comments at top of function) we have to check whether the check run
        // exists on GitHub before we can do anything.
        //
        // Destructive creation would be catastrophic if, for example, our
        // "new" building check run replaced the existing built one because it
        // would look exactly like a transition from built back to building in
        // the GitHub UI. And then the built lambda could run after our
        // building lambda, creating an opportunity for the real node ID to be
        // overwritten with the old one.
        //
        cr.build_id = move (bid);

        // Fetch the check run by name from GitHub.
        //
        pair<optional<gh_check_run>, bool> pr (
            gq_fetch_check_run (iat->token,
                                ts.id,
                                gh_check_run_name (b, &hs),
                                error));

        if (pr.second) // No errors.
        {
          if (!pr.first) // Check run does not exist on GitHub.
          {
            // Assume the most probable cases: build_queued() failed to create
            // the check run or build_building() is running before
            // build_queued(), so creating with building state is
            // correct. (The least likely being that build_built() ran before
            // this, in which case we should create with the built state.)
            //
            // @@ TODO Create with whatever the failed state was if we decide
            //         to store it.
            //
            if (gq_create_check_run (cr,
                                     iat->token,
                                     sd.repository_id, sd.head_sha,
                                     b,
                                     build_state::queued,
                                     hs,
                                     error))
            {
              l3 ([&]{trace << "created check_run { " << cr << " }";});
            }
          }
          else // Check run exists on GitHub.
          {
            if (pr.first->status == gh_to_status (build_state::queued))
            {
              if (scr != nullptr)
              {
                cr = move (*scr);
                cr.state = nullopt;
              }

              if (gq_update_check_run (cr,
                                       iat->token,
                                       sd.repository_id,
                                       pr.first->node_id,
                                       build_state::building,
                                       error))
              {
                l3 ([&]{trace << "updated check_run { " << cr << " }";});
              }
            }
            else
            {
              // Do nothing because the GitHub state is already built so the
              // lambda returned by build_built() will update the database to
              // built.
              //
              return nullptr;
            }
          }
        }
        else // Error communicating with GitHub.
        {
          // Can't tell whether the check run exists GitHub. Make the final
          // decision on whether or not to store nullopt in node_id and state
          // based on what's in the database when the lambda runs
          // (build_built() and its lambda could run in the meantime).
          //
          // @@ TODO Store build_state::building if we start storing failed
          //         state.
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
        // Update existing check run.
        //
        // @@ TODO What if the failed GH update was for built? May end up with
        //         permanently building check run.
        //
        if (!scr->state || scr->state == build_state::queued)
        {
          scr->state = cr.state;
          if (!scr->node_id)
            scr->node_id = move (cr.node_id);
        }
      }
      else
      {
        // Store new check run.
        //
        sd.check_runs.push_back (cr);

        warn << "check run { " << cr << " }: "
             << to_string (build_state::building)
             << " state being persisted before "
             << to_string (build_state::queued);
      }

      return sd.json ();
    };
  }

  function<optional<string> (const tenant_service&)> ci_github::
  build_built (const tenant_service&, const build&,
               const build_hints&,
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
