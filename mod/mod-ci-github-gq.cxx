// file      : mod/mod-ci-github-gq.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-ci-github-gq.hxx>

#include <libbutl/json/parser.hxx>
#include <libbutl/json/serializer.hxx>

#include <mod/mod-ci-github-post.hxx>

using namespace std;
using namespace butl;

namespace brep
{
  // GraphQL serialization functions (see definitions and documentation at the
  // bottom).
  //
  static const string& gq_name (const string&);
  static string        gq_name (string&&);
  static string        gq_str (const string&);
  static string        gq_int (uint64_t);
  static string        gq_bool (bool);
  static const string& gq_enum (const string&);
  static string        gq_enum (string&&);

  [[noreturn]] static void
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
  // The parse_data function should not throw anything but invalid_json_input.
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
  // If the `errors` field is present in the response, error(s) occurred
  // before or during execution of the operation.
  //
  // If the `data` field is not present the errors are request errors which
  // occur before execution and are typically the client's fault.
  //
  // If the `data` field is also present in the response the errors are field
  // errors which occur during execution and are typically the GraphQL
  // endpoint's fault, and some fields in `data` that should not be are likely
  // to be null.
  //
  // Although the spec recommends that the errors field (if present) should
  // come before the data field, GitHub places data before errors. Therefore
  // we need to check that the errors field is not present before parsing the
  // data field as it might contain nulls if errors is present.
  //
  static void
  gq_parse_response (json::parser& p,
                     function<void (json::parser&)> parse_data)
  {
    using event = json::event;

    // True if the data/errors fields are present.
    //
    bool dat (false), err (false);

    // Because the errors field is likely to come before the data field,
    // serialize data to a stringstream and only parse it later once we're
    // sure there are no errors.
    //
    stringstream data; // The value of the data field.

    p.next_expect (event::begin_object);

    while (p.next_expect (event::name, event::end_object))
    {
      if (p.name () == "data")
      {
        dat = true;

        // Serialize the data field to a string.
        //
        // Note that the JSON payload sent by GitHub is not pretty-printed so
        // there is no need to worry about that.
        //
        json::stream_serializer s (data, 0 /* indentation */);

        try
        {
          for (event e: p)
          {
            if (!s.next (e, p.data ()))
              break; // Stop if data object is complete.
          }
        }
        catch (const json::invalid_json_output& e)
        {
          throw_json (p,
                      string ("serializer rejected response 'data' field: ") +
                        e.what ());
        }
      }
      else if (p.name () == "errors")
      {
        // Skip the errors object but don't stop parsing because the error
        // semantics depends on whether or not `data` is present.
        //
        err = true; // Handled below.

        p.next_expect_value_skip ();
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

    if (!err)
    {
      if (!dat)
        throw runtime_error ("no data received from GraphQL endpoint");

      // Parse the data field now that we know there are no errors.
      //
      json::parser dp (data, p.input_name);

      parse_data (dp);
    }
    else
    {
      if (dat)
      {
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
  // Throw invalid_json_input.
  //
  // Example response (only the part we need to parse here):
  //
  //  {
  //    "cr0": {
  //      "checkRun": {
  //        "node_id": "CR_kwDOLc8CoM8AAAAFQ5GqPg",
  //        "name": "libb2/0.98.1+2/x86_64-linux-gnu/linux_debian_12-gcc_13.1-O3/default/dev/0.17.0-a.1",
  //        "status": "QUEUED"
  //      }
  //    },
  //    "cr1": {
  //      "checkRun": {
  //        "node_id": "CR_kwDOLc8CoM8AAAAFQ5GqhQ",
  //        "name": "libb2/0.98.1+2/x86_64-linux-gnu/linux_debian_12-gcc_13.1/default/dev/0.17.0-a.1",
  //        "status": "QUEUED"
  //      }
  //    }
  //  }
  //
  static vector<gh_check_run>
  gq_parse_mutate_check_runs_response (json::parser& p)
  {
    using event = json::event;

    vector<gh_check_run> r;

    gq_parse_response (p, [&r] (json::parser& p)
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

  // Serialize a query that fetches the most recent check runs on a commit.
  //
  static string
  gq_query_get_check_runs (uint64_t ai,      // App id
                           const string& ri, // Repository id
                           const string& ci, // Commit id
                           size_t cn)        // Check run count
  {

    ostringstream os;

    os << "query {"                                                     << '\n';

    // Get the repository node.
    //
    os << "node(id: " << gq_str (ri) << ") {"                           << '\n'
       << "... on Repository {"                                         << '\n';

    // Get the commit object.
    //
    os << "  object(oid: " << gq_str (ci) << ") {"                      << '\n'
       << "  ... on Commit {"                                           << '\n';

    // Get the check suites on the commit, filtering by our app id. (Note that
    // as a result there should never be more than one check suite; see
    // below.)
    //
    os << "    checkSuites(first: 1"                                    << '\n'
       << "                filterBy: {appId: " << gq_int (ai) << "}) {" << '\n'
       << "      edges { node {"                                        << '\n';

    // Get the check suite's last N check runs (last:).
    //
    // Filter by App id because apparently an App can create check runs in
    // another App's check suite.
    //
    // Also ask for the latest check runs only (checkType: LATEST) otherwise
    // we could receive multiple check runs with the same name. Although this
    // appears to be the default it's not documented anywhere so best make it
    // explicit.
    //
    // Note that the selection set (fields to be returned) must match that of
    // the check run mutations (create/update) generated by
    // gq_mutation_{create,update}_check_runs().
    //
    os << "        checkRuns(last: " << gq_int (cn)                     << '\n'
       << "                  filterBy: {appId: " << gq_int (ai)         << '\n'
       << "                             checkType: LATEST}) {"          << '\n'
       << "          edges { node { node_id: id name status } }"        << '\n'
       << "        }" /* checkRuns */                                   << '\n'
       << "      } }" /* node, edges */                                 << '\n'
       << "    }"     /* checkSuites */                                 << '\n'
       << "  }"       /* ... on Commit */                               << '\n'
       << "  }"       /* object */                                      << '\n'
       << "}"         /* ... on Repository */                           << '\n'
       << "}"         /* node */                                        << '\n';

    os << '}'         /* query */                                       << '\n';

    return os.str ();
  }

  // Parse a response to a "get check runs for repository/commit" GraphQL
  // query as constructed by gq_query_get_check_runs().
  //
  // Note that there might be other check suites on this commit but they will
  // all have been created by other apps (GitHub never creates more than one
  // check suite per app). Therefore our query filters by app id and as a
  // result there should never be more than one check suite in the response.
  //
  // Throw invalid_json_input.
  //
  // Example response (only the part we need to parse here):
  //
  // {
  //   "node": {
  //     "object":{
  //       "checkSuites":{
  //         "edges":[
  //            {"node":{
  //               "checkRuns":{
  //                 "edges":[
  //                   {"node":{"id":"CR_kwDOLc8CoM8AAAAImvJPfw",
  //                            "name":"check_run0",
  //                            "status":"QUEUED"}},
  //                   {"node":{"id":"CR_kwDOLc8CoM8AAAAImvJP_Q",
  //                            "name":"check_run1",
  //                            "status":"QUEUED"}}
  //                 ]
  //               }
  //             }
  //           }
  //         ]
  //       }
  //     }
  //   }
  // }
  //
  static vector<gh_check_run>
  gq_parse_get_check_runs_response (json::parser& p)
  {
    using event = json::event;

    vector<gh_check_run> r;

    gq_parse_response (p, [&r] (json::parser& p)
    {
      p.next_expect (event::begin_object); // Outermost {

      p.next_expect_member_object ("node");   // Repository node
      p.next_expect_member_object ("object"); // Commmit
      p.next_expect_member_object ("checkSuites");
      p.next_expect_member_array ("edges"); // Check suites array
      p.next_expect (event::begin_object);  // Check suite outer {
      p.next_expect_member_object ("node");
      p.next_expect_member_object ("checkRuns");
      p.next_expect_member_array ("edges"); // Check runs array

      // Parse the check run elements of the `edges` array. E.g.:
      //
      // {
      //   "node":{
      //     "node_id":"CR_kwDOLc8CoM8AAAAIobBFlA",
      //     "name":"CONCLUSION",
      //     "status":"IN_PROGRESS"
      //   }
      // }
      //
      while (p.next_expect (event::begin_object, event::end_array))
      {
        p.next_expect_name ("node");
        r.emplace_back (p); // Parse check run: { members... }
        p.next_expect (event::end_object);
      }

      p.next_expect (event::end_object); // checkRuns
      p.next_expect (event::end_object); // Check suite node
      p.next_expect (event::end_object); // Check suite outer }
      p.next_expect (event::end_array);  // Check suites edges
      p.next_expect (event::end_object); // checkSuites
      p.next_expect (event::end_object); // Commit
      p.next_expect (event::end_object); // Repository node

      p.next_expect (event::end_object); // Outermost }
    });

    return r;
  }

  // Serialize a GraphQL operation (query/mutation) into a GraphQL request.
  //
  // This is essentially a JSON object with a "query" string member containing
  // the GraphQL operation. For example:
  //
  // { "query": "mutation { cr0:createCheckRun(... }" }
  //
  static string
  gq_serialize_request (const string& o)
  {
    string b;
    json::buffer_serializer s (b);

    s.begin_object ();
    s.member ("query", o);
    s.end_object ();

    return b;
  }

  // Send a GraphQL mutation request `rq` that creates (create=true) or
  // updates (create=false) one or more check runs. The requested build state
  // is taken from each check_run object. Update the check runs in `crs` with
  // the new data (state, node ID if unset, and state_synced). Return false
  // and issue diagnostics if the request failed.
  //
  struct gq_create_data
  {
    uint64_t                        app_id;
    reference_wrapper<const string> repository_id;
    reference_wrapper<const string> head_sha;
  };

  static bool
  gq_mutate_check_runs (const basic_mark& error,
                        check_runs::iterator crs_b,
                        check_runs::iterator crs_e,
                        const string& iat,
                        string rq,
                        const optional<gq_create_data>& create_data)
  {
    size_t crs_n (crs_e - crs_b);

    const char* what (nullptr);
    try
    {
      // Response type which parses a GraphQL response containing multiple
      // check_run objects.
      //
      struct resp
      {
        vector<gh_check_run> check_runs; // Received check runs.

        resp (json::parser& p)
            : check_runs (gq_parse_mutate_check_runs_response (p)) {}

        resp () = default;
      } rs;

      what = create_data ? "create" : "update";
      uint16_t sc (github_post (rs,
                                "graphql", // API Endpoint.
                                strings {"Authorization: Bearer " + iat},
                                move (rq)));

      // Turns out it's not uncommon to not get a reply from GitHub if the
      // number of check runs being created in build_queued() is large. The
      // symptom is a 502 (Bad gateway) reply from GitHub and the theory being
      // that their load balancer drops the connection if the request is not
      // handled within a certain time. Note that if the number of check runs
      // is under 100, they seem to still be created on GitHub, we just don't
      // get the reply (and thus their node ids). So we try to re-query that
      // information.
      //
      optional<uint16_t> sc1;
      if (sc == 502 && create_data)
      {
        what = "re-query";

        // GraphQL query which fetches the most recently-created check runs.
        //
        string rq (gq_serialize_request (
          gq_query_get_check_runs (create_data->app_id,
                                   create_data->repository_id,
                                   create_data->head_sha,
                                   crs_n)));

        // Type that parses the result of the above GraphQL query.
        //
        struct resp
        {
          vector<gh_check_run> check_runs; // Received check runs.

          resp (json::parser& p)
              : check_runs (gq_parse_get_check_runs_response (p)) {}

          resp () = default;
        } rs1;

        sc1 = github_post (rs1,
                           "graphql", // API Endpoint.
                           strings {"Authorization: Bearer " + iat},
                           move (rq));

        if (*sc1 == 200)
        {
          if (rs1.check_runs.size () == crs_n)
          {
            // It's possible GitHub did not create all the checkruns we have
            // requested. In which case it may return some unrelated checkruns
            // (for example, from before re-request). So we verify we got the
            // expected ones.
            //
            size_t i (0);
            for (; i != crs_n; ++i)
            {
              const check_run& cr (*(crs_b + i));
              const gh_check_run& gcr (rs1.check_runs[i]);

              if (cr.name != gcr.name ||
                  cr.state != gh_from_status (gcr.status))
                break;
            }

            if (i == crs_n)
            {
              rs.check_runs = move (rs1.check_runs);

              // Reduce to as-if the create request succeeded.
              //
              what = "create";
              sc = 200;
            }
          }
        }
      }

      if (sc == 200)
      {
        vector<gh_check_run>& rcrs (rs.check_runs);

        if (rcrs.size () == crs_n)
        {
          for (size_t i (0); i != crs_n; ++i)
          {
            check_run& cr (*(crs_b + i));

            // Validate the check run in the response against the build.
            //
            const gh_check_run& rcr (rcrs[i]); // Received check run.

            build_state st (cr.state);                     // Requested state.
            build_state rst (gh_from_status (rcr.status)); // Received state.

            // Note that GitHub won't allow us to change a built check run to
            // any other state (but all other transitions are allowed).
            //
            if (rst != st && rst != build_state::built)
            {
              error << "unexpected check_run status: received '" << rcr.status
                    << "' but expected '" << gh_to_status (st) << '\'';

              return false; // Fail because something is clearly very wrong.
            }

            if (!cr.node_id)
              cr.node_id = move (rcr.node_id);

            cr.state = rst;
            cr.state_synced = (rst == st);
          }

          return true;
        }
        else
          error << "unexpected number of check_run objects in response";
      }
      else
      {
        diag_record dr (error);

        dr << "failed to " << what << " check runs: error HTTP response status "
           << sc;

        if (sc1)
        {
          if (*sc1 != 200)
            dr << error << "failed to re-query check runs: error HTTP "
               << "response status " << *sc1;
          else
            dr << error << "unexpected number of check_run objects in "
               << "re-query response";
        }
      }
    }
    catch (const json::invalid_json_input& e) // struct resp (via github_post())
    {
      // Note: e.name is the GitHub API endpoint.
      //
      error << "malformed JSON in " << what << " response from " << e.name
            << ", line: " << e.line << ", column: " << e.column
            << ", byte offset: " << e.position
            << ", error: " << e;
    }
    catch (const invalid_argument& e) // github_post()
    {
      error << "malformed header(s) in " << what << " response: " << e;
    }
    catch (const system_error& e) // github_post()
    {
      error << "unable to " << what << " check runs (errno=" << e.code ()
            << "): " << e.what ();
    }
    catch (const runtime_error& e) // gq_parse_{mutate,get}_check_runs_response()
    {
      // GitHub response contained error(s) (could be ours or theirs at this
      // point).
      //
      error << "unable to " << what << " check runs: " << e;
    }

    return false;
  }

  // Serialize `createCheckRun` mutations for one or more builds to GraphQL.
  //
  // The check run parameters (names, build states, details_urls, etc.) are
  // taken from each object in `crs`.
  //
  // Note that build results are not supported because we never create
  // multiple check runs in the built state.
  //
  // The details URL argument (`du`) can be empty for queued but not for the
  // other states.
  //
  // Throw invalid_argument if any of the observed check run members are not
  // valid GraphQL values (string, enum, etc).
  //
  static string
  gq_mutation_create_check_runs (const string& ri,           // Repository ID
                                 const string& hs,           // Head SHA
                                 brep::check_runs::iterator crs_b,
                                 brep::check_runs::iterator crs_e)
  {
    ostringstream os;

    os << "mutation {"                                              << '\n';

    // Serialize a `createCheckRun` for each build.
    //
    for (brep::check_runs::iterator crs_i (crs_b); crs_i != crs_e; ++crs_i)
    {
      const check_run& cr (*crs_i);

      assert (cr.state != build_state::built); // Not supported.

      // Ensure details URL and output are non-empty if present.
      //
      assert (!cr.details_url || !cr.details_url->empty ());
      assert (!cr.description ||
              (!cr.description->title.empty () &&
               !cr.description->summary.empty ()));

      string al ("cr" + to_string (crs_i - crs_b)); // Field alias.

      os << gq_name (al) << ":createCheckRun(input: {"              << '\n'
         << "  name: "         << gq_str (cr.name)                  << '\n'
         << "  repositoryId: " << gq_str (ri)                       << '\n'
         << "  headSha: "      << gq_str (hs)                       << '\n'
         << "  status: "       << gq_enum (gh_to_status (cr.state));
      if (cr.details_url)
      {
        os                                                          << '\n';
        os << "  detailsUrl: " << gq_str (*cr.details_url);
      }
      if (cr.description)
      {
        os << "  output: {"                                         << '\n'
           << "    title: "    << gq_str (cr.description->title)    << '\n'
           << "    summary: "  << gq_str (cr.description->summary)  << '\n'
           << "  }";
      }
      os << "})"                                                    << '\n'
        // Specify the selection set (fields to be returned). Note that we
        // rename `id` to `node_id` (using a field alias) for consistency with
        // webhook events and REST API responses.
        //
         << "{"                                                     << '\n'
         << "  checkRun {"                                          << '\n'
         << "    node_id: id"                                       << '\n'
         << "    name"                                              << '\n'
         << "    status"                                            << '\n'
         << "  }"                                                   << '\n'
         << "}"                                                     << '\n';
    }

    os << "}"                                                       << '\n';

    return os.str ();
  }

  // Serialize a `createCheckRun` mutation for a build to GraphQL.
  //
  // The conclusion argument (`co`) is required if the check run status is
  // completed because GitHub does not allow a check run status of completed
  // without a conclusion.
  //
  // The details URL argument (`du`) can be empty for queued but not for the
  // other states.
  //
  // Throw invalid_argument if any of the arguments or observed check run
  // members are not valid GraphQL values (string, enum, etc).
  //
  static string
  gq_mutation_create_check_run (const string& ri,           // Repository ID
                                const string& hs,           // Head SHA
                                const optional<string>& du, // Details URL.
                                const check_run& cr,
                                const string& st,           // Check run status.
                                const string& ti,           // Output title.
                                const string& su,           // Output summary.
                                optional<string> co = nullopt) // Conclusion.
  {
    // Ensure details URL is non-empty if present.
    //
    assert (!du || !du->empty ());

    // Ensure we have conclusion if the status is completed.
    //
    assert (st != "COMPLETED" || co);

    ostringstream os;

    os << "mutation {"                                             << '\n';

    // Serialize a `createCheckRun` for the build.
    //
    os << gq_name ("cr0") << ":createCheckRun(input: {"           << '\n'
       << "  name: "         << gq_str (cr.name)                  << '\n'
       << "  repositoryId: " << gq_str (ri)                       << '\n'
       << "  headSha: "      << gq_str (hs)                       << '\n'
       << "  status: "       << gq_enum (st);
    if (du)
    {
      os                                                          << '\n';
      os << "  detailsUrl: " << gq_str (*du);
    }
    os                                                            << '\n';
    if (co)
      os << "  conclusion: " << gq_enum (*co)                     << '\n';
    os << "  output: {"                                           << '\n'
       << "    title: "    << gq_str (ti)                         << '\n'
       << "    summary: "  << gq_str (su)                         << '\n'
       << "  }";
    os << "})"                                                    << '\n'
      // Specify the selection set (fields to be returned). Note that we
      // rename `id` to `node_id` (using a field alias) for consistency with
      // webhook events and REST API responses.
      //
       << "{"                                                     << '\n'
       << "  checkRun {"                                          << '\n'
       << "    node_id: id"                                       << '\n'
       << "    name"                                              << '\n'
       << "    status"                                            << '\n'
       << "  }"                                                   << '\n'
       << "}"                                                     << '\n';

    os << "}"                                                      << '\n';

    return os.str ();
  }

  // Serialize an `updateCheckRun` mutation for one build to GraphQL.
  //
  // The `br` argument is required if the check run status is completed
  // because GitHub does not allow updating a check run to completed without a
  // conclusion.
  //
  // Throw invalid_argument if any of the arguments are invalid values (of
  // GraphQL types or otherwise).
  //
  static string
  gq_mutation_update_check_run (const string& ri,           // Repository ID.
                                const string& ni,           // Node ID.
                                const string& st,           // Check run status.
                                optional<timestamp> sa,     // Started at.
                                const string& ti,           // Output title.
                                const string& su,           // Output summary.
                                optional<string> co = nullopt) // Conclusion.
  {
    // Ensure we have conclusion if the status is completed.
    //
    assert (st != "COMPLETED" || co);

    ostringstream os;

    os << "mutation {"                                            << '\n'
       << "cr0:updateCheckRun(input: {"                           << '\n'
       << "  checkRunId: "   << gq_str (ni)                       << '\n'
       << "  repositoryId: " << gq_str (ri)                       << '\n'
       << "  status: "       << gq_enum (st);
    if (sa)
    {
      try
      {
        os                                                        << '\n';
        os << "  startedAt: " << gq_str (gh_to_iso8601 (*sa));
      }
      catch (const system_error& e)
      {
        // Translate for simplicity.
        //
        throw invalid_argument ("unable to convert started_at value " +
                                to_string (system_clock::to_time_t (*sa)) +
                                ": " + e.what ());
      }
    }
    os                                                            << '\n';
    if (co)
      os << "  conclusion: " << gq_enum (*co)                     << '\n';
    os << "  output: {"                                           << '\n'
       << "    title: "    << gq_str (ti)                         << '\n'
       << "    summary: "  << gq_str (su)                         << '\n'
       << "  }";
    os << "})"                                                    << '\n'
      // Specify the selection set (fields to be returned). Note that we
      // rename `id` to `node_id` (using a field alias) for consistency with
      // webhook events and REST API responses.
      //
       << "{"                                                     << '\n'
       << "  checkRun {"                                          << '\n'
       << "    node_id: id"                                       << '\n'
       << "    name"                                              << '\n'
       << "    status"                                            << '\n'
       << "  }"                                                   << '\n'
       << "}"                                                     << '\n'
       << "}"                                                     << '\n';

    return os.str ();
  }

  bool
  gq_create_check_runs (const basic_mark& error,
                        brep::check_runs& crs,
                        const string& iat,
                        uint64_t ai,
                        const string& rid,
                        const string& hs,
                        size_t batch)
  {
    assert (batch != 0);

    // No support for result_status so state cannot be built.
    //
#ifndef NDEBUG
    for (const check_run& cr: crs)
      assert (cr.state != build_state::built);
#endif

    // Trying to create a large number of check runs at once does not work.
    // There are two failure modes:
    //
    // 1. Between about 40 - 60 we may get 502 (bad gateway) but the check
    //    runs are still created on GitHub. We handle this case be re-quering
    //    the check runs (see gq_mutate_check_runs() for details).
    //
    // 2. Above about 60 GitHub may not create all the check runs (while still
    //    responding with 502). We handle this here by batching the creation.
    //
    size_t n (crs.size ());
    size_t b (n / batch + (n % batch != 0 ? 1 : 0));
    size_t bn (n / b);

    auto i (crs.begin ());
    for (size_t j (0); j != b; )
    {
      auto e (++j != b ? (i + bn): crs.end ());

      string rq (
        gq_serialize_request (
          gq_mutation_create_check_runs (rid, hs, i, e)));

      if (!gq_mutate_check_runs (error,
                                 i, e,
                                 iat,
                                 move (rq),
                                 gq_create_data {ai, rid, hs}))
        return false;

      i += bn;
    }

    return true;
  }

  bool
  gq_create_check_run (const basic_mark& error,
                       check_run& cr,
                       const string& iat,
                       uint64_t ai,
                       const string& rid,
                       const string& hs,
                       const optional<string>& du,
                       build_state st,
                       string ti, string su)
  {
    // State cannot be built without a conclusion.
    //
    assert (st != build_state::built && !ti.empty () && !su.empty ());

    string rq (
      gq_serialize_request (
        gq_mutation_create_check_run (rid,
                                      hs,
                                      du,
                                      cr,
                                      gh_to_status (st),
                                      move (ti), move (su),
                                      nullopt /* conclusion */)));

    brep::check_runs crs {move (cr)};
    crs[0].state = st;

    bool r (gq_mutate_check_runs (error,
                                  crs.begin (), crs.end (),
                                  iat,
                                  move (rq),
                                  gq_create_data {ai, rid, hs}));

    cr = move (crs[0]);

    return r;
  }

  bool
  gq_create_check_run (const basic_mark& error,
                       check_run& cr,
                       const string& iat,
                       uint64_t ai,
                       const string& rid,
                       const string& hs,
                       const optional<string>& du,
                       gq_built_result br)
  {
    string rq (
      gq_serialize_request (
        gq_mutation_create_check_run (rid,
                                      hs,
                                      du,
                                      cr,
                                      gh_to_status (build_state::built),
                                      move (br.title), move (br.summary),
                                      move (br.conclusion))));

    brep::check_runs crs {move (cr)};
    crs[0].state = build_state::built;

    bool r (gq_mutate_check_runs (error,
                                  crs.begin (), crs.end (),
                                  iat,
                                  move (rq),
                                  gq_create_data {ai, rid, hs}));

    cr = move (crs[0]);

    return r;
  }

  bool
  gq_update_check_run (const basic_mark& error,
                       check_run& cr,
                       const string& iat,
                       const string& rid,
                       const string& nid,
                       build_state st,
                       string ti, string su)
  {
    // State cannot be built without a conclusion.
    //
    assert (st != build_state::built && !ti.empty () && !su.empty ());

    // Set `startedAt` to current time if updating to building.
    //
    optional<timestamp> sa;

    if (st == build_state::building)
      sa = system_clock::now ();

    string rq (
      gq_serialize_request (
        gq_mutation_update_check_run (rid,
                                      nid,
                                      gh_to_status (st),
                                      sa,
                                      move (ti), move (su),
                                      nullopt /* conclusion */)));

    brep::check_runs crs {move (cr)};
    crs[0].state = st;

    bool r (gq_mutate_check_runs (error,
                                  crs.begin (), crs.end (),
                                  iat,
                                  move (rq),
                                  nullopt));

    cr = move (crs[0]);

    return r;
  }

  bool
  gq_update_check_run (const basic_mark& error,
                       check_run& cr,
                       const string& iat,
                       const string& rid,
                       const string& nid,
                       gq_built_result br)
  {
    string rq (
      gq_serialize_request (
        gq_mutation_update_check_run (rid,
                                      nid,
                                      gh_to_status (build_state::built),
                                      nullopt /* startedAt */,
                                      move (br.title), move (br.summary),
                                      move (br.conclusion))));

    brep::check_runs crs {move (cr)};
    crs[0].state = build_state::built;

    bool r (gq_mutate_check_runs (error,
                                  crs.begin (), crs.end (),
                                  iat,
                                  move (rq),
                                  nullopt));

    cr = move (crs[0]);

    return r;
  }

  // Serialize a GraphQL query that fetches a pull request from GitHub.
  //
  // Throw invalid_argument if the node id is not a valid GraphQL string.
  //
  static string
  gq_query_pr_mergeability (const string& nid)
  {
    ostringstream os;

    os << "query {"                                              << '\n'
       << "  node(id:" << gq_str (nid) << ") {"                  << '\n'
       << "    ... on PullRequest {"                             << '\n'
       << "      headRefOid"                                     << '\n'
       << "      mergeStateStatus"                               << '\n'
       << "      mergeable"                                      << '\n'
       << "      potentialMergeCommit { oid }"                   << '\n'
       << "    }"                                                << '\n'
       << "  }"                                                  << '\n'
       << "}"                                                    << '\n';

    return os.str ();
  }

  optional<gq_pr_pre_check_info>
  gq_fetch_pull_request_pre_check_info (const basic_mark& error,
                                        const string& iat,
                                        const string& nid)
  {
    // Let invalid_argument from gq_query_pr_mergeability() propagate.
    //
    string rq (gq_serialize_request (gq_query_pr_mergeability (nid)));

    try
    {
      // Response parser.
      //
      struct resp
      {
        // True if the pull request was found (i.e., the node ID was valid).
        //
        bool found = false;

        // Non-fatal error message issued during the parse.
        //
        string parse_error;

        // The response value. Absent if the merge commit is still being
        // generated.
        //
        optional<gq_pr_pre_check_info> r;

        resp (json::parser& p)
        {
          using event = json::event;

          gq_parse_response (p, [this] (json::parser& p)
          {
            p.next_expect (event::begin_object);

            if (p.next_expect_member_object_null ("node"))
            {
              found = true;

              string hs (p.next_expect_member_string ("headRefOid"));
              string ms (p.next_expect_member_string ("mergeStateStatus"));
              string ma (p.next_expect_member_string ("mergeable"));

              if (ms == "BEHIND")
              {
                // The PR head branch is not up to date with the PR base
                // branch.
                //
                // Note that we can only get here if the head-not-behind
                // protection rule is active on the PR base branch.
                //
                r = {move (hs), true, nullopt};
              }
              else if (ma == "MERGEABLE")
              {
                p.next_expect_member_object ("potentialMergeCommit");
                string oid (p.next_expect_member_string ("oid"));
                p.next_expect (event::end_object);

                r = {move (hs), false, move (oid)};
              }
              else
              {
                if (ma == "CONFLICTING")
                  r = {move (hs), false, nullopt};
                else if (ma == "UNKNOWN")
                  ; // Still being generated; leave r absent.
                else
                  throw_json (p, "unexpected mergeable value '" + ma + '\'');
              }

              if (!r || !r->merge_commit_sha)
              {
                // Skip the merge commit ID if it has not yet been extracted
                // (in which case it should be null).
                //
                p.next_expect_name ("potentialMergeCommit");
                p.next_expect_value_skip ();
              }

              p.next_expect (event::end_object);   // node
            }

            p.next_expect (event::end_object);
          });
        }

        resp () = default;
      } rs;

      uint16_t sc (github_post (rs,
                                "graphql", // API Endpoint.
                                strings {"Authorization: Bearer " + iat},
                                move (rq)));

      if (sc == 200)
      {
        if (!rs.found)
          error << "pull request '" << nid << "' not found";
        else if (!rs.parse_error.empty ())
          error << rs.parse_error;

        return rs.r;
      }
      else
        error << "failed to fetch pull request: error HTTP response status "
              << sc;
    }
    catch (const json::invalid_json_input& e) // struct resp (via github_post())
    {
      // Note: e.name is the GitHub API endpoint.
      //
      error << "malformed JSON in response from " << e.name << ", line: "
            << e.line << ", column: " << e.column << ", byte offset: "
            << e.position << ", error: " << e;
    }
    catch (const invalid_argument& e) // github_post()
    {
      error << "malformed header(s) in response: " << e;
    }
    catch (const system_error& e) // github_post()
    {
      error << "unable to fetch pull request (errno=" << e.code () << "): "
            << e.what ();
    }
    catch (const runtime_error& e) // struct resp
    {
      // GitHub response contained error(s) (could be ours or theirs at this
      // point).
      //
      error << "unable to fetch pull request: " << e;
    }

    return nullopt;
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

  static string
  gq_name (string&& v)
  {
    gq_name (v);
    return move (v);
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
  static string
  gq_int (uint64_t v)
  {
    string b;
    json::buffer_serializer s (b);
    s.value (v);
    return b;
  }

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

  static string
  gq_enum (string&& v)
  {
    gq_enum (v);
    return move (v);
  }

}
