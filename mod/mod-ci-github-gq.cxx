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
  static string        gq_str (const string&);
  static string        gq_bool (bool);
  static const string& gq_enum (const string&);

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
  gq_parse_response_check_runs (json::parser& p)
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

  // Send a GraphQL mutation request `rq` that operates on one or more check
  // runs. Update the check runs in `crs` with the new state and the node ID
  // if unset. Return false and issue diagnostics if the request failed.
  //
  static bool
  gq_mutate_check_runs (const basic_mark& error,
                        vector<check_run>& crs,
                        const string& iat,
                        string rq,
                        build_state st) noexcept
  {
    vector<gh_check_run> rcrs;

    try
    {
      // Response type which parses a GraphQL response containing multiple
      // check_run objects.
      //
      struct resp
      {
        vector<gh_check_run> check_runs; // Received check runs.

        resp (json::parser& p)
            : check_runs (gq_parse_response_check_runs (p)) {}

        resp () = default;
      } rs;

      uint16_t sc (github_post (rs,
                                "graphql", // API Endpoint.
                                strings {"Authorization: Bearer " + iat},
                                move (rq)));

      if (sc == 200)
      {
        rcrs = move (rs.check_runs);

        if (rcrs.size () == crs.size ())
        {
          for (size_t i (0); i != rcrs.size (); ++i)
          {
            // Validate the check run in the response against the build.
            //
            const gh_check_run& rcr (rcrs[i]); // Received check run.

            build_state rst (gh_from_status (rcr.status)); // Received state.

            // Note that GitHub won't allow us to change a built check run to
            // any other state (but all other transitions are allowed).
            //
            // @@ Are we handling the case where the resulting state (built)
            //    differs from what we expect?
            //
            // @@@ Does built-to-built transition updates status?
            //
            if (rst != st && rst != build_state::built)
            {
              error << "unexpected check_run status: received '" << rcr.status
                    << "' but expected '" << gh_to_status (st) << '\'';

              return false; // Fail because something is clearly very wrong.
            }

            check_run& cr (crs[i]);

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
        error << "failed to update check run: error HTTP response status "
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
      error << "unable to mutate check runs (errno=" << e.code () << "): "
            << e.what ();
    }
    catch (const runtime_error& e) // From gq_parse_response_check_runs().
    {
      // GitHub response contained error(s) (could be ours or theirs at this
      // point).
      //
      error << "unable to mutate check runs: " << e;
    }

    return false;
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

  // Serialize `createCheckRun` mutations for one or more builds to GraphQL.
  //
  // The conclusion argument (`co`) is required if the build_state is built
  // because GitHub does not allow a check run status of completed without a
  // conclusion.
  //
  // The details URL argument (`du`) can be empty for queued but not for the
  // other states.
  //
  static string
  gq_mutation_create_check_runs (const string& ri,           // Repository ID
                                 const string& hs,           // Head SHA
                                 const optional<string>& du, // Details URL.
                                 const vector<check_run>& crs,
                                 const string& st,           // Check run status.
                                 optional<gq_built_result> br = nullopt)
  {
    // Ensure details URL is non-empty if present.
    //
    assert (!du || !du->empty ());

    ostringstream os;

    os << "mutation {"                                              << '\n';

    // Serialize a `createCheckRun` for each build.
    //
    for (size_t i (0); i != crs.size (); ++i)
    {
      string al ("cr" + to_string (i)); // Field alias.

      os << gq_name (al) << ":createCheckRun(input: {"              << '\n'
         << "  name: "         << gq_str (crs[i].name)              << '\n'
         << "  repositoryId: " << gq_str (ri)                       << '\n'
         << "  headSha: "      << gq_str (hs)                       << '\n'
         << "  status: "       << gq_enum (st);
      if (du)
      {
        os                                                          << '\n';
        os << "  detailsUrl: " << gq_str (*du);
      }
      if (br)
      {
        os                                                          << '\n';
        os << "  conclusion: " << gq_enum (br->conclusion)          << '\n'
           << "  output: {"                                         << '\n'
           << "    title: "    << gq_str (br->title)                << '\n'
           << "    summary: "  << gq_str (br->summary)              << '\n'
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

  // Serialize an `updateCheckRun` mutation for one build to GraphQL.
  //
  // The `co` (conclusion) argument is required if the build_state is built
  // because GitHub does not allow updating a check run to completed without a
  // conclusion.
  //
  static string
  gq_mutation_update_check_run (const string& ri,           // Repository ID.
                                const string& ni,           // Node ID.
                                const optional<string>& du, // Details URL.
                                const string& st,           // Check run status.
                                optional<timestamp> sa,     // Started at.
                                optional<gq_built_result> br)
  {
    // Ensure details URL is non-empty if present.
    //
    assert (!du || !du->empty ());

    ostringstream os;

    os << "mutation {"                                            << '\n'
       << "cr0:updateCheckRun(input: {"                           << '\n'
       << "  checkRunId: "   << gq_str (ni)                       << '\n'
       << "  repositoryId: " << gq_str (ri)                       << '\n'
       << "  status: "       << gq_enum (st);
    if (sa)
    {
      os                                                          << '\n';
      os << "  startedAt: " << gq_str (gh_to_iso8601 (*sa));
    }
    if (du)
    {
      os                                                          << '\n';
      os << "  detailsUrl: " << gq_str (*du);
    }
    if (br)
    {
      os                                                          << '\n';
      os << "  conclusion: " << gq_enum (br->conclusion)          << '\n'
         << "  output: {"                                         << '\n'
         << "    title: "    << gq_str (br->title)                << '\n'
         << "    summary: "  << gq_str (br->summary)              << '\n'
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
       << "}"                                                     << '\n'
       << "}"                                                     << '\n';

    return os.str ();
  }

  bool
  gq_create_check_runs (const basic_mark& error,
                        vector<check_run>& crs,
                        const string& iat,
                        const string& rid,
                        const string& hs,
                        build_state st)
  {
    // No support for result_status so state cannot be built.
    //
    assert (st != build_state::built);

    // Empty details URL because it's not available until building.
    //
    string rq (
      gq_serialize_request (gq_mutation_create_check_runs (rid,
                                                           hs,
                                                           nullopt,
                                                           crs,
                                                           gh_to_status (st))));

    return gq_mutate_check_runs (error, crs, iat, move (rq), st);
  }

  bool
  gq_create_check_run (const basic_mark& error,
                       check_run& cr,
                       const string& iat,
                       const string& rid,
                       const string& hs,
                       const optional<string>& du,
                       build_state st,
                       optional<gq_built_result> br)
  {
    // Must have a result if state is built.
    //
    assert (st != build_state::built || br);

    vector<check_run> crs {move (cr)};

    string rq (
      gq_serialize_request (
        gq_mutation_create_check_runs (rid,
                                       hs,
                                       du,
                                       crs,
                                       gh_to_status (st),
                                       move (br))));

    bool r (gq_mutate_check_runs (error, crs, iat, move (rq), st));

    cr = move (crs[0]);

    return r;
  }

  bool
  gq_update_check_run (const basic_mark& error,
                       check_run& cr,
                       const string& iat,
                       const string& rid,
                       const string& nid,
                       const optional<string>& du,
                       build_state st,
                       optional<gq_built_result> br)
  {
    // Must have a result if state is built.
    //
    assert (st != build_state::built || br);

    // Set `startedAt` to current time if updating to building.
    //
    optional<timestamp> sa;

    if (st == build_state::building)
      sa = system_clock::now ();

    string rq (
      gq_serialize_request (
        gq_mutation_update_check_run (rid,
                                      nid,
                                      du,
                                      gh_to_status (st),
                                      sa,
                                      move (br))));

    vector<check_run> crs {move (cr)};

    bool r (gq_mutate_check_runs (error, crs, iat, move (rq), st));

    cr = move (crs[0]);

    return r;
  }

  // Serialize a GraphQL query that fetches a pull request from GitHub.
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
                if (ma == "UNKNOWN")
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
      error << "unable to fetch pull request (errno=" << e.code () << "): "
            << e.what ();
    }
    catch (const runtime_error& e) // From response type's parsing constructor.
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
  // @@ TODO: dangerous API.
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
  // @@ TODO: dangerous API.
  //
  static const string&
  gq_enum (const string& v)
  {
    if (v == "true" || v == "false" || v == "null")
      throw invalid_argument ("invalid GraphQL enum value: '" + v + '\'');

    return gq_name (v);
  }
}
