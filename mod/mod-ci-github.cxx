// file      : mod/mod-ci-github.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-ci-github.hxx>

#include <libbutl/curl.hxx>
#include <libbutl/json/parser.hxx>

#include <mod/jwt.hxx>
#include <mod/module-options.hxx>

#include <stdexcept>
#include <iostream>

// @@ TODO
//
//    Building CI checks with a GitHub App
//    https://docs.github.com/en/apps/creating-github-apps/writing-code-for-a-github-app/building-ci-checks-with-a-github-app
//

// @@ TODO Best practices
//
//    Webhooks:
//    https://docs.github.com/en/webhooks/using-webhooks/best-practices-for-using-webhooks
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

// @@ Authenticating to use the API
//
//    There are three types of authentication:
//
//    1) Authenticating as an app. Used to access parts of the API concerning
//       the app itself such as getting the list of installations. (Need to
//       authenticate as an app as part of authenticating as an app
//       installation.)
//
//    2) Authenticating as an app installation (on a user or organisation
//       account). Used to access resources belonging to the user/repository
//       or organisation the app is installed in.
//
//    3) Authenticating as a user. Used to perform actions as the user.
//
//    We need to authenticate as an app installation (2).
//
//    How to authenticate as an app installation
//
//    Reference:
//    https://docs.github.com/en/apps/creating-github-apps/authenticating-with-a-github-app/authenticating-as-a-github-app-installation
//
//    The final authentication token we need is an installation access token
//    (IAT), valid for one hour, which we will pass in the `Authentication`
//    header of our Github API requests:
//
//      Authorization: Bearer <INSTALLATION_ACCESS_TOKEN>
//
//    To generate an IAT:
//
//    - Generate a JSON Web Token (JWT)
//
//    - Get the installation ID. This will be included in the webhook request
//      in our case
//
//    - Send a POST to /app/installations/<INSTALLATION_ID>/access_tokens
//      which includes the JWT (`Authorization: Bearer <JWT>`). The response
//      will include the IAT. Can pass the name of the repository included in
//      the webhook request to restrict access, otherwise we get access to all
//      repos covered by the installation if installed on an organisation for
//      example.
//

using namespace std;
using namespace butl;
using namespace web;
using namespace brep::cli;

namespace brep
{
  // GitHub-specific types.
  //
  // Note that having this types directly in brep causes clashes (e.g., for
  // the repository name).
  //
  namespace gh
  {
    // The "check_suite" object within a check_quite webhook request.
    //
    struct check_suite
    {
      uint64_t id;
      string head_branch;
      string head_sha;
      string before;
      string after;

      explicit
      check_suite (json::parser&);

      check_suite () = default;
    };

    struct repository
    {
      string name;
      string full_name;
      string default_branch;

      explicit
      repository (json::parser&);

      repository () = default;
    };

    struct installation
    {
      uint64_t id;

      explicit
      installation (json::parser&);

      installation () = default;
    };

    struct check_suite_event
    {
      string action;
      gh::check_suite check_suite;
      gh::repository repository;
      gh::installation installation;

      explicit
      check_suite_event (json::parser&);

      check_suite_event () = default;
    };

    struct installation_access_token
    {
      string token;
      timestamp expires_at;

      explicit
      installation_access_token (json::parser&);

      installation_access_token () = default;
    };

    static ostream&
    operator<< (ostream&, const check_suite&);

    static ostream&
    operator<< (ostream&, const repository&);

    static ostream&
    operator<< (ostream&, const installation&);

    static ostream&
    operator<< (ostream&, const check_suite_event&);

    static ostream&
    operator<< (ostream&, const installation_access_token&);
  }

  using namespace gh;

  ci_github::
  ci_github (const ci_github& r)
      : handler (r),
        options_ (r.initialized_ ? r.options_ : nullptr)
  {
  }

  void ci_github::
  init (scanner& s)
  {
    options_ = make_shared<options::ci_github> (
      s, unknown_mode::fail, unknown_mode::fail);
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
  template<typename T>
  static uint16_t
  github_post (T& rs, const string& ep, const strings& hdrs)
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

      curl c (nullfd,
              path ("-"), // Write response to curl::in.
              process::pipe (errp.in.get (), move (errp.out)),
              curl::post,
              curl::flags::no_fail,
              "https://api.github.com/" + ep,
              "--include", // Output response headers for status code.
              "--header", "Accept: application/vnd.github+json",
              "--header", "X-GitHub-Api-Version: 2022-11-28",
              move (hdr_opts));

      ifdstream err (move (errp.in));

      // Parse the HTTP response.
      //
      int sc; // Status code.
      try
      {
        // Note: re-open in/out so that they get automatically closed on
        // exception.
        //
        ifdstream in (c.in.release (), fdstream_mode::skip);

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

  bool ci_github::
  handle (request& rq, response&)
  {
    using namespace bpkg;

    HANDLER_DIAG;

    // @@ TODO
    if (false)
      throw invalid_request (404, "CI request submission disabled");

    // Process headers.
    //
    string event;
    {
      bool content_type (false);

      for (const name_value& h: rq.headers ())
      {
        if (icasecmp (h.name, "x-github-delivery") == 0)
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
    }

    // There is an event (specified in the x-github-event header) and each event
    // contains a bunch of actions (specified in the JSON request body).
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
        json::parser p (rq.content (64 * 1024), "check_suite webhook");

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
      }
      else if (cs.action == "rerequested")
      {
        // Someone manually requested to re-run the check runs in this check
        // suite.
      }
      else if (cs.action == "completed")
      {
        // GitHub thinks that "all the check runs in this check suite have
        // completed and a conclusion is available". Looks like this one we
        // ignore?
      }
      else
      {
        // Ignore unknown actions by sending a 200 response with empty body
        // but also log as an error since we want to notice new actions.
        //
        error << "unknown action '" << cs.action << "' in check_suite webhook";

        return true;
      }

      cout << "<check_suite webhook>" << endl << cs << endl;

      string jwt;
      try
      {
        // Set token's "issued at" time 60 seconds in the past to combat clock
        // drift (as recommended by GitHub).
        //
        jwt = gen_jwt (
          *options_,
          options_->ci_github_app_private_key (),
          to_string (options_->ci_github_app_id ()),
          chrono::seconds (options_->ci_github_jwt_validity_period ()),
          chrono::seconds (60));

        cout << "JWT: " << jwt << endl;
      }
      catch (const system_error& e)
      {
        fail << "unable to generate JWT (errno=" << e.code () << "): " << e;
      }

      // Authenticate to GitHub as an app installation.
      //
      installation_access_token iat;
      try
      {
        // API endpoint.
        //
        string ep ("app/installations/" + to_string (cs.installation.id) +
                   "/access_tokens");

        int sc (github_post (iat, ep, strings {"Authorization: Bearer " + jwt}));

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
          fail << "unable to get installation access token: "
               << "error HTTP response status " << sc;
        }
      }
      catch (const json::invalid_json_input& e)
      {
        // Note: e.name is the GitHub API endpoint.
        //
        fail << "malformed JSON in response from " << e.name << ", line: "
             << e.line << ", column: " << e.column << ", byte offset: "
             << e.position << ", error: " << e;
      }
      catch (const invalid_argument& e)
      {
        fail << "malformed header(s) in response: " << e;
      }
      catch (const system_error& e)
      {
        fail << "unable to get installation access token (errno=" << e.code ()
             << "): " << e.what ();
      }

      cout << endl << "<installation_access_token>" << endl << iat << endl;

      return true;
    }
    else if (event == "pull_request")
    {
      throw invalid_request (501, "pull request events not implemented yet");
    }
    else
      throw invalid_request (400, "unexpected event: '" + event + "'");
  }

  using event = json::event;

  // Throw invalid_json_input when a required member `m` is missing from a
  // JSON object `o`.
  //
  [[noreturn]] static void
  missing_member (const json::parser& p, const char* o, const char* m)
  {
    throw json::invalid_json_input (
      p.input_name,
      p.line (), p.column (), p.position (),
      o + string (" object is missing member `") + m + "`");
  }

  // check_suite
  //
  gh::check_suite::
  check_suite (json::parser& p)
  {
    p.next_expect (event::begin_object);

    bool i (false), hb (false), hs (false), bf (false), at (false);

    // Skip unknown/uninteresting members.
    //
    while (p.next_expect (event::name, event::end_object))
    {
      auto c = [&p] (bool& v, const char* s)
      {
        return p.name () == s ? (v = true) : false;
      };

      if      (c (i, "id"))           id = p.next_expect_number<uint64_t> ();
      else if (c (hb, "head_branch")) head_branch = p.next_expect_string ();
      else if (c (hs, "head_sha"))    head_sha = p.next_expect_string ();
      else if (c (bf, "before"))      before = p.next_expect_string ();
      else if (c (at, "after"))       after = p.next_expect_string ();
      else p.next_expect_value_skip ();
    }

    if (!i)  missing_member (p, "check_suite", "id");
    if (!hb) missing_member (p, "check_suite", "head_branch");
    if (!hs) missing_member (p, "check_suite", "head_sha");
    if (!bf) missing_member (p, "check_suite", "before");
    if (!at) missing_member (p, "check_suite", "after");
  }

  static ostream&
  gh::operator<< (ostream& os, const check_suite& cs)
  {
    os << "id: " << cs.id << endl
       << "head_branch: " << cs.head_branch << endl
       << "head_sha: " << cs.head_sha << endl
       << "before: " << cs.before << endl
       << "after: " << cs.after << endl;

    return os;
  }

  // repository
  //
  gh::repository::
  repository (json::parser& p)
  {
    p.next_expect (event::begin_object);

    bool nm (false), fn (false), db (false);

    // Skip unknown/uninteresting members.
    //
    while (p.next_expect (event::name, event::end_object))
    {
      auto c = [&p] (bool& v, const char* s)
      {
        return p.name () == s ? (v = true) : false;
      };

      if      (c (nm, "name"))           name = p.next_expect_string ();
      else if (c (fn, "full_name"))      full_name = p.next_expect_string ();
      else if (c (db, "default_branch")) default_branch = p.next_expect_string ();
      else p.next_expect_value_skip ();
    }

    if (!nm) missing_member (p, "repository", "name");
    if (!fn) missing_member (p, "repository", "full_name");
    if (!db) missing_member (p, "repository", "default_branch");
  }

  static ostream&
  gh::operator<< (ostream& os, const repository& rep)
  {
    os << "name: " << rep.name << endl
       << "full_name: " << rep.full_name << endl
       << "default_branch: " << rep.default_branch << endl;

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

  static ostream&
  gh::operator<< (ostream& os, const installation& i)
  {
    os << "id: " << i.id << endl;

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

  static ostream&
  gh::operator<< (ostream& os, const check_suite_event& cs)
  {
    os << "action: " << cs.action << endl;
    os << "<check_suite>" << endl << cs.check_suite;
    os << "<repository>" << endl << cs.repository;
    os << "<installation>" << endl << cs.installation;

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
      else if (c (ea, "expires_at"))
      {
        const string& s (p.next_expect_string ());
        expires_at = from_string (s.c_str (), "%Y-%m-%dT%TZ", false /* local */);
      }
      else p.next_expect_value_skip ();
    }

    if (!tk) missing_member (p, "installation_access_token", "token");
    if (!ea) missing_member (p, "installation_access_token", "expires_at");
  }

  static ostream&
  gh::operator<< (ostream& os, const installation_access_token& t)
  {
    os << "token: " << t.token << endl;
    os << "expires_at: ";
    butl::operator<< (os, t.expires_at) << endl;

    return os;
  }
}
