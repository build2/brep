// file      : mod/mod-ci-github.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-ci-github.hxx>

#include <libbutl/curl.hxx>
#include <libbutl/json/parser.hxx>
#include <libbutl/json/serializer.hxx>

#include <mod/jwt.hxx>
#include <mod/hmac.hxx>
#include <mod/module-options.hxx>

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
  using namespace gh;

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
      check_suite_event cs;
      try
      {
        json::parser p (body.data (), body.size (), "check_suite event");

        cs = check_suite_event (p);
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

  // Service data associated with the tenant/check suite.
  //
  // It is always a top-level JSON object and the first member is always the
  // schema version.
  //
  struct service_data
  {
    // The data schema version. Note: must be first member in the object.
    //
    uint64_t version = 1;

    // Check suite-global data.
    //
    installation_access_token installation_access;

    uint64_t installation_id;
    // @@ TODO Rename to repository_node_id.
    //
    string repository_id; // GitHub-internal opaque repository id.

    string head_sha;

    // Absent state means we were unable to (conclusively) notify GitHub about
    // the last state transition (e.g., due to a transient network error). The
    // "conclusively" part means that the notification may or may not have
    // gone through. Note: node_id can be absent for the same reason.
    //
    struct check_run
    {
      string                build_id; // Full build id.
      optional<string>      node_id;  // GitHub id.
      optional<build_state> state;

      string
      state_string () const
      {
        return state ? to_string (*state) : "null";
      }
    };
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
  operator<< (ostream&, const service_data::check_run&);

  bool ci_github::
  handle_check_suite_request (check_suite_event cs)
  {
    HANDLER_DIAG;

    l3 ([&]{trace << "check_suite event { " << cs << " }";});

    optional<string> jwt (generate_jwt (trace, error));
    if (!jwt)
      throw server_error ();

    optional<installation_access_token> iat (
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

  // GraphQL serialization functions.
  //
  // The GraphQL spec:
  //   https://spec.graphql.org/
  //
  // The GitHub GraphQL API reference:
  //   https://docs.github.com/en/graphql/reference/
  //

  // Check that a string is a valid GraphQL name.
  //
  // GraphQL names can contain only alphanumeric characters and underscores
  // and cannot begin with a digit (so basically a C identifier).
  //
  // Return the name or throw invalid_argument if it is invalid.
  //
  static const string&
  gq_name (const string& v)
  {
    if (v.empty () || digit (v[0]))
      throw invalid_argument ("invalid GraphQL name: '" + v + '\'');

    for (char c: v)
    {
      if (!alnum (c) && c != '_')
      {
        throw invalid_argument ("invalid character in GraphQL name: '" + c +
                                '\'');
      }
    }

    return v;
  }

  // Serialize a string to GraphQL.
  //
  // Return the serialized string or throw invalid_argument if the string is
  // invalid.
  //
  static string
  gq_str (const string& v)
  {
    // GraphQL strings are the same as JSON strings so we use the JSON
    // serializer.
    //
    string b;
    json::buffer_serializer s (b);

    try
    {
      s.value (v);
    }
    catch (const json::invalid_json_output&)
    {
      throw invalid_argument ("invalid GraphQL string: '" + v + '\'');
    }

    return b;
  }

  // Serialize an int to GraphQL.
  //
#if 0
  static string
  gq_int (uint64_t v)
  {
    string b;
    json::buffer_serializer s (b);
    s.value (v);
    return b;
  }
#endif

  // Serialize a boolean to GraphQL.
  //
  static inline string
  gq_bool (bool v)
  {
    return v ? "true" : "false";
  }

  // Check that a string is a valid GraphQL enum value.
  //
  // GraphQL enum values can be any GraphQL name except for `true`, `false`,
  // or `null`.
  //
  // Return the enum value or throw invalid_argument if it is invalid.
  //
  static const string&
  gq_enum (const string& v)
  {
    if (v == "true" || v == "false" || v == "null")
      throw invalid_argument ("invalid GraphQL enum value: '" + v + '\'');

    return gq_name (v);
  }

  // Create a check_run name from a build. If the second argument is not
  // NULL, return an abbreviated id if possible.
  //
  static string
  check_run_name (const build& b,
                  const tenant_service_base::build_hints* bh = nullptr)
  {
    string r;

    if (bh == nullptr || !bh->single_package_version)
    {
      r += b.package_name.string ();
      r += '/';
      r += b.package_version.string ();
      r += '/';
    }

    r += b.target_config_name;
    r += '/';
    r += b.target.string ();
    r += '/';

    if (bh == nullptr || !bh->single_package_config)
    {
      r += b.package_config_name;
      r += '/';
    }

    r += b.toolchain_name;
    r += '-';
    r += b.toolchain_version.string ();

    return r;
  }

  // Return the GitHub check run status corresponding to a build_state.
  //
  static const string&
  to_string_gh (build_state st)
  {
    static const string sts[] {"QUEUED", "IN_PROGRESS", "COMPLETED"};

    return sts[static_cast<size_t> (st)];
  }

  // Return the build_state corresponding to a GitHub check run status
  // string. Throw invalid_argument if the passed status was invalid.
  //
  static build_state
  from_string_gh (const string& s)
  {
    if      (s == "QUEUED")      return build_state::queued;
    else if (s == "IN_PROGRESS") return build_state::building;
    else if (s == "COMPLETED")   return build_state::built;
    else
      throw invalid_argument ("invalid GitHub check run status: '" + s +
                              '\'');
  }

  // Serialize `createCheckRun` mutations for one or more builds to GraphQL.
  //
  static string
  create_check_runs (const string& ri, // Repository ID
                     const string& hs, // Head SHA
                     const vector<reference_wrapper<const build>>& bs,
                     build_state st,
                     const tenant_service_base::build_hints* bh = nullptr)
  {
    ostringstream os;

    os << "mutation {"                                              << '\n';

    // Serialize a `createCheckRun` for each build.
    //
    for (size_t i (0); i != bs.size (); ++i)
    {
      const build& b (bs[i]);

      string al ("cr" + to_string (i)); // Field alias.

      // Check run name.
      //
      string nm (check_run_name (b, bh));

      os << gq_name (al) << ":createCheckRun(input: {"              << '\n'
         << "  name: "         << gq_str (nm) << ','                << '\n'
         << "  repositoryId: " << gq_str (ri) << ','                << '\n'
         << "  headSha: "      << gq_str (hs) << ','                << '\n'
         << "  status: "       << gq_enum (to_string_gh (st))       << '\n'
         << "})"                                                    << '\n'
        // Specify the selection set (fields to be returned).
        //
         << "{"                                                     << '\n'
         << "  checkRun {"                                          << '\n'
         << "    id,"                                               << '\n'
         << "    name,"                                             << '\n'
         << "    status"                                            << '\n'
         << "  }"                                                   << '\n'
         << "}"                                                     << '\n';
    }

    os << "}"                                                       << '\n';

    return os.str ();
  }

  // Serialize an `updateCheckRun` mutation for one build to GraphQL.
  //
  // @@ TODO Support conclusion, output, etc.
  //
  static string
  update_check_run (const string& ri, // Repository ID
                    const string& ci, // Check run node_id
                    build_state st)
  {
    ostringstream os;

    os << "mutation {"                                            << '\n'
       << "cr0:updateCheckRun(input: {"                           << '\n'
       << "  checkRunId: "   << gq_str (ci) << ','                << '\n'
       << "  repositoryId: " << gq_str (ri) << ','                << '\n'
       << "  status: "       << gq_enum (to_string_gh (st))       << '\n'
       << "})"                                                    << '\n'
      // Specify the selection set (fields to be returned).
      //
       << "{"                                                     << '\n'
       << "  checkRun {"                                          << '\n'
       << "    id,"                                               << '\n'
       << "    name,"                                             << '\n'
       << "    status"                                            << '\n'
       << "  }"                                                   << '\n'
       << "}"                                                     << '\n'
       << "}"                                                     << '\n';

    return os.str ();
  }

  // Serialize a GraphQL operation (query/mutation) into a GraphQL request.
  //
  // This is essentially a JSON object with a "query" string member containing
  // the GraphQL operation. For example:
  //
  // { "query": "mutation { cr0:createCheckRun(... }" }
  //
  static string
  graphql_request (const string& o)
  {
    string b;
    json::buffer_serializer s (b);

    s.begin_object ();
    s.member ("query", o);
    s.end_object ();

    return b;
  }

  [[noreturn]] void
  throw_json (json::parser& p, const string& m)
  {
    throw json::invalid_json_input (
      p.input_name,
      p.line (), p.column (), p.position (),
      m);
  }

  // Parse a JSON-serialized GraphQL response.
  //
  // Throw runtime_error if the response indicated errors and
  // invalid_json_input if the GitHub response contained invalid JSON.
  //
  // The response format is defined in the GraphQL spec:
  // https://spec.graphql.org/October2021/#sec-Response.
  //
  // Example response:
  //
  // {
  //   "data": {...},
  //   "errors": {...}
  // }
  //
  // The contents of `data`, including its opening and closing braces, are
  // parsed by the `parse_data` function.
  //
  // @@ TODO errors comes before data in GitHub's responses.
  //
  static void
  parse_graphql_response (json::parser& p,
                          function<void (json::parser&)> parse_data)
  {
    using event = json::event;

    // True if the data/errors fields are present.
    //
    // Although the spec merely recommends that the `errors` field, if
    // present, comes before the `data` field, assume it always does because
    // letting the client parse data in the presence of field errors
    // (unexpected nulls) would not make sense.
    //
    bool dat (false), err (false);

    p.next_expect (event::begin_object);

    while (p.next_expect (event::name, event::end_object))
    {
      if (p.name () == "data")
      {
        dat = true;

        // Currently we're not handling fields that are null due to field
        // errors (see below for details) so don't parse any further.
        //
        if (err)
          break;

        parse_data (p);
      }
      else if (p.name () == "errors")
      {
        // Don't stop parsing because the error semantics depends on whether
        // or not `data` is present.
        //
        err = true; // Handled below.
      }
      else
      {
        // The spec says the response will never contain any top-level fields
        // other than data, errors, and extensions.
        //
        if (p.name () != "extensions")
        {
          throw_json (p,
                      "unexpected top-level GraphQL response field: '" +
                          p.name () + '\'');
        }

        p.next_expect_value_skip ();
      }
    }

    // If the `errors` field was present in the response, error(s) occurred
    // before or during execution of the operation.
    //
    // If the `data` field was not present the errors are request errors which
    // occur before execution and are typically the client's fault.
    //
    // If the `data` field was also present in the response the errors are
    // field errors which occur during execution and are typically the GraphQL
    // endpoint's fault, and some fields in `data` that should not be are
    // likely to be null.
    //
    if (err)
    {
      if (dat)
      {
        // @@ TODO: Consider parsing partial data?
        //
        throw runtime_error ("field error(s) received from GraphQL endpoint; "
                             "incomplete data received");
      }
      else
        throw runtime_error ("request error(s) received from GraphQL endpoint");
    }
  }

  // Parse a response to a check_run GraphQL mutation such as `createCheckRun`
  // or `updateCheckRun`.
  //
  // Example response (only the part we need to parse here):
  //
  //  {
  //    "cr0": {
  //      "checkRun": {
  //        "id": "CR_kwDOLc8CoM8AAAAFQ5GqPg",
  //        "name": "libb2/0.98.1+2/x86_64-linux-gnu/linux_debian_12-gcc_13.1-O3/default/dev/0.17.0-a.1",
  //        "status": "QUEUED"
  //      }
  //    },
  //    "cr1": {
  //      "checkRun": {
  //        "id": "CR_kwDOLc8CoM8AAAAFQ5GqhQ",
  //        "name": "libb2/0.98.1+2/x86_64-linux-gnu/linux_debian_12-gcc_13.1/default/dev/0.17.0-a.1",
  //        "status": "QUEUED"
  //      }
  //    }
  //  }
  //
  // @@ TODO Handle response errors properly.
  //
  static vector<check_run>
  parse_check_runs_response (json::parser& p)
  {
    using event = json::event;

    vector<check_run> r;

    parse_graphql_response (
      p,
      [&r] (json::parser& p)
      {
        p.next_expect (event::begin_object);

        // Parse the "cr0".."crN" members (field aliases).
        //
        while (p.next_expect (event::name, event::end_object))
        {
          // Parse `"crN": { "checkRun":`.
          //
          if (p.name () != "cr" + to_string (r.size ()))
            throw_json (p, "unexpected field alias: '" + p.name () + '\'');
          p.next_expect (event::begin_object);
          p.next_expect_name ("checkRun");

          r.emplace_back (p); // Parse the check_run object.

          p.next_expect (event::end_object); // Parse end of crN object.
        }
      });

    // Our requests always operate on at least one check run so if there were
    // none in the data field something went wrong.
    //
    if (r.empty ())
      throw_json (p, "data object is empty");

    return r;
  }

  // Send a POST request to the GitHub API endpoint `ep`, parse GitHub's JSON
  // response into `rs` (only for 200 codes), and return the HTTP status code.
  //
  // The endpoint `ep` should not have a leading slash.
  //
  // Pass additional HTTP headers in `hdrs`. For example:
  //
  //   "HeaderName: header value"
  //
  // Throw invalid_argument if unable to parse the response headers,
  // invalid_json_input (derived from invalid_argument) if unable to parse the
  // response body, and system_error in other cases.
  //
  template <typename T>
  static uint16_t
  github_post (T& rs,
               const string& ep,
               const strings& hdrs,
               const string& body = "")
  {
    // Convert the header values to curl header option/value pairs.
    //
    strings hdr_opts;

    for (const string& h: hdrs)
    {
      hdr_opts.push_back ("--header");
      hdr_opts.push_back (h);
    }

    // Run curl.
    //
    try
    {
      // Pass --include to print the HTTP status line (followed by the response
      // headers) so that we can get the response status code.
      //
      // Suppress the --fail option which causes curl to exit with status 22
      // in case of an error HTTP response status code (>= 400) otherwise we
      // can't get the status code.
      //
      // Note that butl::curl also adds --location to make curl follow redirects
      // (which is recommended by GitHub).
      //
      // The API version `2022-11-28` is the only one currently supported. If
      // the X-GitHub-Api-Version header is not passed this version will be
      // chosen by default.
      //
      fdpipe errp (fdopen_pipe ()); // stderr pipe.

      curl c (path ("-"), // Read input from curl::out.
              path ("-"), // Write response to curl::in.
              process::pipe (errp.in.get (), move (errp.out)),
              curl::post,
              curl::flags::no_fail,
              "https://api.github.com/" + ep,
              "--no-fail", // Don't fail if response status code >= 400.
              "--include", // Output response headers for status code.
              "--header", "Accept: application/vnd.github+json",
              "--header", "X-GitHub-Api-Version: 2022-11-28",
              move (hdr_opts));

      ifdstream err (move (errp.in));

      // Parse the HTTP response.
      //
      uint16_t sc; // Status code.
      try
      {
        // Note: re-open in/out so that they get automatically closed on
        // exception.
        //
        ifdstream in (c.in.release (), fdstream_mode::skip);
        ofdstream out (c.out.release ());

        // Write request body to out.
        //
        if (!body.empty ())
          out << body;
        out.close ();

        sc = curl::read_http_status (in).code; // May throw invalid_argument.

        // Parse the response body if the status code is in the 200 range.
        //
        if (sc >= 200 && sc < 300)
        {
          // Use endpoint name as input name (useful to have it propagated
          // in exceptions).
          //
          json::parser p (in, ep /* name */);
          rs = T (p);
        }

        in.close ();
      }
      catch (const io_error& e)
      {
        // If the process exits with non-zero status, assume the IO error is due
        // to that and fall through.
        //
        if (c.wait ())
        {
          throw_generic_error (
            e.code ().value (),
            (string ("unable to read curl stdout: ") + e.what ()).c_str ());
        }
      }
      catch (const json::invalid_json_input&)
      {
        // If the process exits with non-zero status, assume the JSON error is
        // due to that and fall through.
        //
        if (c.wait ())
          throw;
      }

      if (!c.wait ())
      {
        string et (err.read_text ());
        throw_generic_error (EINVAL,
                             ("non-zero curl exit status: " + et).c_str ());
      }

      err.close ();

      return sc;
    }
    catch (const process_error& e)
    {
      throw_generic_error (
        e.code ().value (),
        (string ("unable to execute curl:") + e.what ()).c_str ());
    }
    catch (const io_error& e)
    {
      // Unable to read diagnostics from stderr.
      //
      throw_generic_error (
        e.code ().value (),
        (string ("unable to read curl stderr : ") + e.what ()).c_str ());
    }
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
    vector<service_data::check_run> crs; // Parallel to bs.

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
      string bid (check_run_name (b)); // Full Build ID.

      const service_data::check_run* scr (sd.find_check_run (bid));

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

    // Queue a check_run for each build.
    //
    string rq (
      graphql_request (
        queue_check_runs (sd.repository_id, sd.head_sha, bs, &hs)));

    // What if we could not notify GitHub about some check runs due to, say, a
    // transient network? In this case we save them with the absent state
    // hoping for things to improve when we try to issue building or built
    // notifications.

    // Get a new installation access token if the current one has expired.
    //
    const installation_access_token* iat (nullptr);
    optional<installation_access_token> new_iat;

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
    try
    {
      // Response type which parses a GraphQL response containing multiple
      // check_run objects.
      //
      struct resp
      {
        vector<check_run> check_runs; // Received check runs.

        resp (json::parser& p) : check_runs (parse_check_runs_response (p)) {}

        resp () = default;
      } rs;

      uint16_t sc (
        github_post (
          rs,
          "graphql", // API Endpoint.
          strings {"Authorization: Bearer " + iat->token},
          move (rq)));

      if (sc == 200)
      {
        if (rs.check_runs.size () == bs.size ())
        {
          // Validate the check runs in the response against the builds.
          //
          for (size_t i (0); i != rs.check_runs.size (); ++i)
          {
            const build& b (bs[i]);
            const check_run& rcr (rs.check_runs[i]); // Received check run.

            if (rcr.name != check_run_name (b, &hs))
              error << "unexpected check_run name: '" + rcr.name + '\'';
            else if (rcr.status != "QUEUED")
              error << "unexpected check_run status: '" + rcr.status + '\'';
            else
            {
              l3 ([&]{trace << "check_run { " << rcr << " }";});

              service_data::check_run& cr (crs[i]);

              if (!cr.node_id)
                cr.node_id = move (rcr.node_id);

              cr.state = build_state::queued;
            }
          }
        }
        else
          error << "unexpected number of check_run objects in response";
      }
      else
        error << "failed to queue check runs: error HTTP response status "
              << sc;
    }
    catch (const json::invalid_json_input& e)
    {
      // Note: e.name is the GitHub API endpoint.
      //
      error << "malformed JSON in response from " << e.name << ", line: "
            << e.line << ", column: " << e.column << ", byte offset: "
            << e.position << ", error: " << e;
    }
    catch (const invalid_argument& e)
    {
      error << "malformed header(s) in response: " << e;
    }
    catch (const system_error& e)
    {
      error << "unable to queue check runs (errno=" << e.code () << "): "
            << e.what ();
    }
    catch (const runtime_error& e) // From parse_check_runs_response().
    {
      // GitHub response contained error(s) (could be ours or theirs at this
      // point).
      //
      error << "unable to queue check runs: " << e;
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
        const service_data::check_run& cr (crs[i]);

        // Note that this service data may not be the same as what we observed
        // in the build_queued() function above. For example, some check runs
        // that we have queued may have already transitioned to building. So
        // we skip any check runs that are already present.
        //
        if (service_data::check_run* scr = sd.find_check_run (cr.build_id))
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
    // check run object has been persistent in the service data (see the
    // returned by build_queued() lambda for details). So we may try to create
    // a new check run but find out that it's actually already exist on the
    // GitHub side. In this case we will somehow need to get the node_id for
    // the existing check run and re-try but this time updating instead of
    // creating.

    return nullptr;
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
  optional<installation_access_token> ci_github::
  obtain_installation_access_token (uint64_t iid,
                                    string jwt,
                                    const basic_mark& error) const
  {
    installation_access_token iat;
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

  static string
  to_iso8601 (timestamp t)
  {
    return butl::to_string (t,
                            "%Y-%m-%dT%TZ",
                            false /* special */,
                            false /* local */);
  }

  static timestamp
  from_iso8601 (const string& s)
  {
    return butl::from_string (s.c_str (), "%Y-%m-%dT%TZ", false /* local */);
  }

  using event = json::event;

  service_data::
  service_data (const string& json)
  {
    json::parser p (json.data (), json.size (), "service_data");

    p.next_expect (event::begin_object);

    // Throw if the schema version is not supported.
    //
    version = p.next_expect_member_number<uint64_t> ("version");
    if (version != 1)
    {
      throw invalid_argument ("unsupported service_data schema version: " +
                              to_string (version));
    }

    // Installation access token.
    //
    p.next_expect_member_object ("installation_access");
    installation_access.token = p.next_expect_member_string ("token");
    installation_access.expires_at =
        from_iso8601 (p.next_expect_member_string ("expires_at"));
    p.next_expect (event::end_object);

    installation_id =
        p.next_expect_member_number<uint64_t> ("installation_id");
    repository_id = p.next_expect_member_string ("repository_id");
    head_sha = p.next_expect_member_string ("head_sha");

    p.next_expect_member_array ("check_runs");
    while (p.next_expect (event::begin_object, event::end_array))
    {
      string bid (p.next_expect_member_string ("build_id"));

      optional<string> nid;
      {
        string* v (p.next_expect_member_string_null ("node_id"));
        if (v != nullptr)
          nid = *v;
      }

      optional<build_state> s;
      {
        string* v (p.next_expect_member_string_null ("state"));
        if (v != nullptr)
          s = to_build_state (*v);
      }

      check_runs.emplace_back (move (bid), move (nid), s);

      p.next_expect (event::end_object);
    }

    p.next_expect (event::end_object);
  }

  service_data::
  service_data (string iat_tok,
                timestamp iat_ea,
                uint64_t iid,
                string rid,
                string hs)
      : installation_access (move (iat_tok), iat_ea),
        installation_id (iid),
        repository_id (move (rid)),
        head_sha (move (hs))
  {
  }

  string service_data::
  json () const
  {
    string b;
    json::buffer_serializer s (b);

    s.begin_object ();

    s.member ("version", 1);

    // Installation access token.
    //
    s.member_begin_object ("installation_access");
    s.member ("token", installation_access.token);
    s.member ("expires_at", to_iso8601 (installation_access.expires_at));
    s.end_object ();

    s.member ("installation_id", installation_id);
    s.member ("repository_id", repository_id);
    s.member ("head_sha", head_sha);

    s.member_begin_array ("check_runs");
    for (const check_run& cr: check_runs)
    {
      s.begin_object ();
      s.member ("build_id", cr.build_id);

      s.member_name ("node_id");
      if (cr.node_id)
        s.value (*cr.node_id);
      else
        s.value (nullptr);

      s.member_name ("state");
      if (cr.state)
        s.value (to_string (*cr.state));
      else
        s.value (nullptr);

      s.end_object ();
    }
    s.end_array ();

    s.end_object ();

    return b;
  }

  service_data::check_run* service_data::
  find_check_run (const string& bid)
  {
    for (check_run& cr: check_runs)
    {
      if (cr.build_id == bid)
        return &cr;
    }
    return nullptr;
  }

  ostream&
  operator<< (ostream& os, const service_data::check_run& cr)
  {
    os << "node_id: " << cr.node_id.value_or ("null")
       << ", build_id: " << cr.build_id
       << ", state: " << (cr.state ? to_string (*cr.state) : "null");

    return os;
  }

  // The rest is GitHub request/response type parsing and printing.
  //

  // Throw invalid_json_input when a required member `m` is missing from a
  // JSON object `o`.
  //
  [[noreturn]] static void
  missing_member (const json::parser& p, const char* o, const char* m)
  {
    throw json::invalid_json_input (
      p.input_name,
      p.line (), p.column (), p.position (),
      o + string (" object is missing member '") + m + '\'');
  }

  // check_suite
  //
  gh::check_suite::
  check_suite (json::parser& p)
  {
    p.next_expect (event::begin_object);

    bool ni (false), hb (false), hs (false), bf (false), at (false);

    // Skip unknown/uninteresting members.
    //
    while (p.next_expect (event::name, event::end_object))
    {
      auto c = [&p] (bool& v, const char* s)
      {
        return p.name () == s ? (v = true) : false;
      };

      if      (c (ni, "node_id"))     node_id = p.next_expect_string ();
      else if (c (hb, "head_branch")) head_branch = p.next_expect_string ();
      else if (c (hs, "head_sha"))    head_sha = p.next_expect_string ();
      else if (c (bf, "before"))      before = p.next_expect_string ();
      else if (c (at, "after"))       after = p.next_expect_string ();
      else p.next_expect_value_skip ();
    }

    if (!ni) missing_member (p, "check_suite", "node_id");
    if (!hb) missing_member (p, "check_suite", "head_branch");
    if (!hs) missing_member (p, "check_suite", "head_sha");
    if (!bf) missing_member (p, "check_suite", "before");
    if (!at) missing_member (p, "check_suite", "after");
  }

  ostream& gh::
  operator<< (ostream& os, const check_suite& cs)
  {
    os << "node_id: " << cs.node_id
       << ", head_branch: " << cs.head_branch
       << ", head_sha: " << cs.head_sha
       << ", before: " << cs.before
       << ", after: " << cs.after;

    return os;
  }

  // check_run
  //
  gh::check_run::
  check_run (json::parser& p)
  {
    p.next_expect (event::begin_object);

    bool ni (false), nm (false), st (false);

    while (p.next_expect (event::name, event::end_object))
    {
      auto c = [&p] (bool& v, const char* s)
      {
        return p.name () == s ? (v = true) : false;
      };

      if      (c (ni, "id"))     node_id = p.next_expect_string ();
      else if (c (nm, "name"))   name = p.next_expect_string ();
      else if (c (st, "status")) status = p.next_expect_string ();
    }

    if (!ni) missing_member (p, "check_run", "id");
    if (!nm) missing_member (p, "check_run", "name");
    if (!st) missing_member (p, "check_run", "status");
  }

  ostream& gh::
  operator<< (ostream& os, const check_run& cr)
  {
    os << "node_id: " << cr.node_id
       << ", name: " << cr.name
       << ", status: " << cr.status;

    return os;
  }

  // repository
  //
  gh::repository::
  repository (json::parser& p)
  {
    p.next_expect (event::begin_object);

    bool ni (false), nm (false), fn (false), db (false), cu (false);

    // Skip unknown/uninteresting members.
    //
    while (p.next_expect (event::name, event::end_object))
    {
      auto c = [&p] (bool& v, const char* s)
      {
        return p.name () == s ? (v = true) : false;
      };

      if      (c (ni, "node_id"))        node_id = p.next_expect_string ();
      else if (c (nm, "name"))           name = p.next_expect_string ();
      else if (c (fn, "full_name"))      full_name = p.next_expect_string ();
      else if (c (db, "default_branch")) default_branch = p.next_expect_string ();
      else if (c (cu, "clone_url"))      clone_url = p.next_expect_string ();
      else p.next_expect_value_skip ();
    }

    if (!ni) missing_member (p, "repository", "node_id");
    if (!nm) missing_member (p, "repository", "name");
    if (!fn) missing_member (p, "repository", "full_name");
    if (!db) missing_member (p, "repository", "default_branch");
    if (!cu) missing_member (p, "repository", "clone_url");
  }

  ostream& gh::
  operator<< (ostream& os, const repository& rep)
  {
    os << "node_id: " << rep.node_id
       << ", name: " << rep.name
       << ", full_name: " << rep.full_name
       << ", default_branch: " << rep.default_branch
       << ", clone_url: " << rep.clone_url;

    return os;
  }

  // installation
  //
  gh::installation::
  installation (json::parser& p)
  {
    p.next_expect (event::begin_object);

    bool i (false);

    // Skip unknown/uninteresting members.
    //
    while (p.next_expect (event::name, event::end_object))
    {
      auto c = [&p] (bool& v, const char* s)
      {
        return p.name () == s ? (v = true) : false;
      };

      if (c (i, "id")) id = p.next_expect_number<uint64_t> ();
      else p.next_expect_value_skip ();
    }

    if (!i) missing_member (p, "installation", "id");
  }

  ostream& gh::
  operator<< (ostream& os, const installation& i)
  {
    os << "id: " << i.id;

    return os;
  }

  // check_suite_event
  //
  gh::check_suite_event::
  check_suite_event (json::parser& p)
  {
    p.next_expect (event::begin_object);

    bool ac (false), cs (false), rp (false), in (false);

    // Skip unknown/uninteresting members.
    //
    while (p.next_expect (event::name, event::end_object))
    {
      auto c = [&p] (bool& v, const char* s)
      {
        return p.name () == s ? (v = true) : false;
      };

      if      (c (ac, "action"))       action = p.next_expect_string ();
      else if (c (cs, "check_suite"))  check_suite = gh::check_suite (p);
      else if (c (rp, "repository"))   repository = gh::repository (p);
      else if (c (in, "installation")) installation = gh::installation (p);
      else p.next_expect_value_skip ();
    }

    if (!ac) missing_member (p, "check_suite_event", "action");
    if (!cs) missing_member (p, "check_suite_event", "check_suite");
    if (!rp) missing_member (p, "check_suite_event", "repository");
    if (!in) missing_member (p, "check_suite_event", "installation");
  }

  ostream& gh::
  operator<< (ostream& os, const check_suite_event& cs)
  {
    os << "action: " << cs.action;
    os << ", check_suite { " << cs.check_suite << " }";
    os << ", repository { "  << cs.repository << " }";
    os << ", installation { " << cs.installation << " }";

    return os;
  }

  // installation_access_token
  //
  // Example JSON:
  //
  // {
  //   "token": "ghs_Py7TPcsmsITeVCAWeVtD8RQs8eSos71O5Nzp",
  //   "expires_at": "2024-02-15T16:16:38Z",
  //   ...
  // }
  //
  gh::installation_access_token::
  installation_access_token (json::parser& p)
  {
    p.next_expect (event::begin_object);

    bool tk (false), ea (false);

    // Skip unknown/uninteresting members.
    //
    while (p.next_expect (event::name, event::end_object))
    {
      auto c = [&p] (bool& v, const char* s)
      {
        return p.name () == s ? (v = true) : false;
      };

      if      (c (tk, "token"))      token = p.next_expect_string ();
      else if (c (ea, "expires_at")) expires_at = from_iso8601 (p.next_expect_string ());
      else p.next_expect_value_skip ();
    }

    if (!tk) missing_member (p, "installation_access_token", "token");
    if (!ea) missing_member (p, "installation_access_token", "expires_at");
  }

  gh::installation_access_token::
  installation_access_token (string tk, timestamp ea)
      : token (move (tk)), expires_at (ea)
  {
  }

  ostream& gh::
  operator<< (ostream& os, const installation_access_token& t)
  {
    os << "token: " << t.token << ", expires_at: ";
    butl::operator<< (os, t.expires_at);

    return os;
  }
}
