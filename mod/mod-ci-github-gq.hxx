// file      : mod/mod-ci-github-gq.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_CI_GITHUB_GQ_HXX
#define MOD_MOD_CI_GITHUB_GQ_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/mod-ci-github-gh.hxx>
#include <mod/mod-ci-github-service-data.hxx>


namespace brep
{
  // GraphQL functions (all start with gq_).
  //

  // @@ TODO:

  gq_create_check_run ();
  gq_update_check_run ();

  // @@ TODO Pass error, trace in same order everywhere.

  // Fetch from GitHub the check run with the specified name (hints-shortened
  // build ID).
  //
  // Return the check run or nullopt if no such check run exists.
  //
  // In case of error diagnostics will be issued and false returned in second.
  //
  // Note that the existence of more than one check run with the same name is
  // considered an error and reported as such. The API docs imply that there
  // can be more than one check run with the same name in a check suite, but
  // the observed behavior is that creating a check run destroys the extant
  // one, leaving only the new one with different node ID.
  //
  pair<optional<gh::check_run>, bool>
  gq_fetch_check_run (const string& iat,
                      const string& check_suite_id,
                      const string& cr_name,
                      const basic_mark& error) noexcept
  {
    try
    {
      // Example request:
      //
      // query {
      //   node(id: "CS_kwDOLc8CoM8AAAAFQPQYEw") {
      //     ... on CheckSuite {
      //       checkRuns(last: 100, filterBy: {checkName: "linux_debian_..."}) {
      //         totalCount,
      //         edges {
      //           node {
      //             id, name, status
      //           }
      //         }
      //       }
      //     }
      //   }
      // }
      //
      // This request does the following:
      //
      // - Look up the check suite by node ID ("direct node lookup"). This
      //   returns a Node (GraphQL interface).
      //
      // - Get to the concrete CheckSuite type by using a GraphQL "inline
      //   fragment" (`... on CheckSuite`).
      //
      // - Get the check suite's check runs
      //   - Filter by the sought name
      //   - Return only two check runs, just enough to be able to tell
      //     whether there are more than one check runs with this name (which
      //     is an error).
      //
      // - Return the id, name, and status fields from the matching check run
      //   objects.
      //
      string rq;
      {
        ostringstream os;

        os << "query {"                                               << '\n';

        os << "node(id: " << gq_str (check_suite_id) << ") {"         << '\n'
           << "  ... on CheckSuite {"                                 << '\n'
           << "    checkRuns(last: 2,"                                << '\n'
           << "              filterBy: {"                             << '\n'
           <<                  "checkName: " << gq_str (cr_name)      << '\n'
           << "              })"                                      << '\n'
          // Specify the selection set (fields to be returned). Note that
          // edges and node are mandatory.
          //
           << "    {"                                                 << '\n'
           << "      totalCount,"                                     << '\n'
           << "      edges {"                                         << '\n'
           << "        node {"                                        << '\n'
           << "          id, name, status"                            << '\n'
           << "        }"                                             << '\n'
           << "      }"                                               << '\n'
           << "    }"                                                 << '\n'
           << "  }"                                                   << '\n'
           << "}"                                                     << '\n';

        os << "}"                                                     << '\n';

        rq = os.str ();
      }

      // Example response (the part we need to parse here, at least):
      //
      //   {
      //     "node": {
      //       "checkRuns": {
      //         "totalCount": 1,
      //         "edges": [
      //           {
      //             "node": {
      //               "id": "CR_kwDOLc8CoM8AAAAFgeoweg",
      //               "name": "linux_debian_...",
      //               "status": "IN_PROGRESS"
      //             }
      //           }
      //         ]
      //       }
      //     }
      //   }
      //
      struct resp
      {
        optional<check_run> cr;
        size_t cr_count = 0;

        resp (json::parser& p)
        {
          using event = json::event;

          parse_graphql_response (p, [this] (json::parser& p)
          {
            p.next_expect (event::begin_object);
            p.next_expect_member_object ("node");
            p.next_expect_member_object ("checkRuns");

            cr_count = p.next_expect_member_number<size_t> ("totalCount");

            p.next_expect_member_array ("edges");

            for (size_t i (0); i != cr_count; ++i)
            {
              p.next_expect (event::begin_object);
              p.next_expect_name ("node");
              check_run cr (p);
              p.next_expect (event::end_object);

              if (i == 0)
                this->cr = move (cr);
            }

            p.next_expect (event::end_array);  // edges
            p.next_expect (event::end_object); // checkRuns
            p.next_expect (event::end_object); // node
            p.next_expect (event::end_object);
          });
        }

        resp () = default;
      } rs;

      uint16_t sc (github_post (rs,
                                "graphql",
                                strings {"Authorization: Bearer " + iat},
                                graphql_request (rq)));

      if (sc == 200)
      {
        if (rs.cr_count <= 1)
          return {rs.cr, true};
        else
        {
          error << "unexpected number of check runs (" << rs.cr_count
                << ") in response";
        }
      }
      else
        error << "failed to get check run by name: error HTTP "
              << "response status " << sc;
    }
    catch (const json::invalid_json_input& e)
    {
      // Note: e.name is the GitHub API endpoint.
      //
      error << "malformed JSON in response from " << e.name
            << ", line: " << e.line << ", column: " << e.column
            << ", byte offset: " << e.position << ", error: " << e;
    }
    catch (const invalid_argument& e)
    {
      error << "malformed header(s) in response: " << e;
    }
    catch (const system_error& e)
    {
      error << "unable to get check run by name (errno=" << e.code ()
            << "): " << e.what ();
    }
    catch (const std::exception& e)
    {
      error << "unable to get check run by name: " << e.what ();
    }

    return {nullopt, false};
  }
}

#endif // MOD_MOD_CI_GITHUB_GQ_HXX
