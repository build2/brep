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

brep::ci_github::
ci_github (const ci_github& r)
    : handler (r),
      options_ (r.initialized_ ? r.options_ : nullptr)
{
}

void brep::ci_github::
init (scanner& s)
{
  options_ = make_shared<options::ci_github> (
    s, unknown_mode::fail, unknown_mode::fail);
}

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
  string        action;
  ::check_suite check_suite;
  ::repository  repository;
  ::installation installation;

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

// Send a POST request to the GitHub API endpoint `ep`, parse GitHub's JSON
// response into `rs`, and return the HTTP status code.
//
// The endpoint `ep` should not have a leading slash.
//
// Pass additional HTTP headers in `hdrs`. For example:
//
//   "HeaderName: header value"
//
// @@ TMP Presumably we'll be factoring most of this function into something
//        like github_send(curl::method_type, ...).
//
template<typename T>
static int
github_post (T& rs, const string& ep, const brep::strings& hdrs)
{
  // Convert the header values to curl header option/value pairs.
  //
  brep::strings hdr_opts;

  for (const string& h: hdrs)
  {
    hdr_opts.push_back ("--header");
    hdr_opts.push_back (h);
  }

  // Run curl.
  //
  try
  {
    // Use the --write-out option to get curl to print the HTTP response
    // status code after the HTTP response body (see below for more details
    // and an example).
    //
    // The API version `2022-11-28` is the only one currently supported and if
    // the X-GitHub-Api-Version header is not passed this version will be
    // chosen by default.
    //
    // @@ TMP Although this request does not have a body, can't pass a nullfd
    //        stdin because it will cause butl::curl to fail if the method is
    //        POST.
    //
    curl c (path ("-"),
            path ("-"), // Write response to curl::in.
            2,
            curl::post, "https://api.github.com/" + ep,
            "--write-out", "{\"brep_http_status\": %{http_code}}\n",
            "--header", "Accept: application/vnd.github+json",
            "--header", "X-GitHub-Api-Version: 2022-11-28",
            move (hdr_opts));

    // Parse the HTTP response.
    //
    int sc; // Status code.
    try
    {
      c.out.close (); // No input required.

      // The output is expected to contain two JSON values: the HTTP response
      // body and the HTTP status code we added with --write-out above. For
      // example:
      //
      //  {
      //    "id": 12345,
      //    "name": "foo"
      //  }
      //  { "brep_http_status": 201 }
      //
      // Name the status code so that we don't accidentally parse some other
      // value.
      //
      // Note that GitHub API error response bodies also consist of a single
      // JSON object so the format will be the same in both cases.
      //
      json::parser p (c.in, ep, true /* multi_value */, "\n");

      // Parse the response body (first JSON value).
      //
      rs = T (p);

      p.next (); // Skip the value-separating nullopt.

      // Parse the HTTP response status code (second JSON value).
      //
      p.next_expect (json::event::begin_object);
      sc = p.next_expect_member_number<int> ("brep_http_status");
      p.next_expect (json::event::end_object);

      c.in.close ();
    }
    catch (const brep::io_error& e)
    {
      // IO failure, child exit status doesn't matter. Just wait for the
      // process completion and throw.
      //
      c.wait ();

      throw_generic_error (
          e.code ().value (),
          (string ("unable to communicate with curl: ") + e.what ())
              .c_str ());
    }
    catch (const json::invalid_json_input& e)
    {
      if (!c.wait ())
      {
        throw_generic_error (
            EINVAL,
            ("curl failed with " + to_string (*c.exit)).c_str ());
      }

      throw runtime_error (
          (string ("malformed JSON response from GitHub: ") + e.what ())
              .c_str ());
    }

    // @@ TMP The odds of this failing are probably slim given that we parsed
    //        the JSON output successfully.
    //
    if (!c.wait ())
    {
      throw_generic_error (
        EINVAL,
        ("curl failed with " + to_string (*c.exit)).c_str ());
    }

    return sc;
  }
  catch (const process_error& e)
  {
    throw_generic_error (
        e.code ().value (),
        (string ("unable to execute curl:") + e.what ()).c_str ());
  }
}

bool brep::ci_github::
handle (request& rq, response& rs)
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
  try
  {
    if (event == "check_suite")
    {
      json::parser p (rq.content (64 * 1024), "check_suite webhook");

      check_suite_event cs (p);

      // @@ TODO: log and ignore unknown.
      //
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
        throw invalid_request (400, "unsupported action: " + cs.action);

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
            chrono::minutes (options_->ci_github_jwt_validity_period ()),
            chrono::seconds (60));

        cout << "JWT: " << jwt << endl;
      }
      catch (const system_error& e)
      {
        fail << "unable to generate JWT: [" << e.code () << "] " << e.what ();
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

        int sc (
            github_post (iat, ep, strings {"Authorization: Bearer " + jwt}));

        // Possible response status codes from the access_tokens endpoint:
        //
        // 201 Created
        // 401 Requires authentication
        // 403 Forbidden
        // 404 Resource not found
        // 422 Validation failed, or the endpoint has been spammed.
        //
        if (sc != 201)
        {
          throw runtime_error ("error status code received from GitHub: " +
                               to_string (sc));
        }
      }
      catch (const system_error& e)
      {
        fail << "unable to get installation access token: [" << e.code ()
             << "] " << e.what ();
      }

      cout << "<installation_access_token>" << endl << iat << endl;

      return true;
    }
    else if (event == "pull_request")
    {
      throw invalid_request (501, "pull request events not implemented yet");
    }
    else
      throw invalid_request (400, "unexpected event: '" + event + "'");
  }
  catch (const json::invalid_json_input& e)
  {
    // @@ TODO: should we write more detailed diagnostics to log? Maybe we
    //    should do this for all unsuccessful calls to respond().
    //
    // Note: these exceptions end up in the apache error log.
    //
    //  @@ TMP Actually I was wrong, these do not end up in any logs. Pretty
    //         sure I saw them go there but they're definitely not anymore.
    //
    throw invalid_request (400, "malformed JSON in request body");
  }
}

using event = json::event;

// check_suite
//
check_suite::
check_suite (json::parser& p)
{
  p.next_expect (event::begin_object);

  // Skip unknown/uninteresting members.
  //
  while (p.next_expect (event::name, event::end_object))
  {
    const string& n (p.name ());

    if      (n == "id")          id = p.next_expect_number<uint64_t> ();
    else if (n == "head_branch") head_branch = p.next_expect_string ();
    else if (n == "head_sha")    head_sha = p.next_expect_string ();
    else if (n == "before")      before = p.next_expect_string ();
    else if (n == "after")       after = p.next_expect_string ();
    else p.next_expect_value_skip ();
  }
}

static ostream&
operator<< (ostream& os, const check_suite& cs)
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
repository::
repository (json::parser& p)
{
  p.next_expect (event::begin_object);

  // Skip unknown/uninteresting members.
  //
  while (p.next_expect (event::name, event::end_object))
  {
    const string& n (p.name ());

    if      (n == "name")           name = p.next_expect_string ();
    else if (n == "full_name")      full_name = p.next_expect_string ();
    else if (n == "default_branch") default_branch = p.next_expect_string ();
    else p.next_expect_value_skip ();
  }
}

static ostream&
operator<< (ostream& os, const repository& rep)
{
  os << "name: " << rep.name << endl
     << "full_name: " << rep.full_name << endl
     << "default_branch: " << rep.default_branch << endl;

  return os;
}

// installation

installation::
installation (json::parser& p)
{
  p.next_expect (event::begin_object);

  // Skip unknown/uninteresting members.
  //
  while (p.next_expect (event::name, event::end_object))
  {
    const string& n (p.name ());

    if (n == "id") id = p.next_expect_number<uint64_t> ();
    else p.next_expect_value_skip ();
  }
}

static ostream&
operator<< (ostream& os, const installation& i)
{
  os << "id: " << i.id << endl;

  return os;
}

// check_suite_event
//
check_suite_event::
check_suite_event (json::parser& p)
{
  p.next_expect (event::begin_object);

  // Skip unknown/uninteresting members.
  //
  while (p.next_expect (event::name, event::end_object))
  {
    const string& n (p.name ());

    if      (n == "action")         action = p.next_expect_string ();
    else if (n == "check_suite")    check_suite = ::check_suite (p);
    else if (n == "repository")     repository = ::repository (p);
    else if (n == "installation")   installation = ::installation (p);
    else p.next_expect_value_skip ();
  }
}

static ostream&
operator<< (ostream& os, const check_suite_event& cs)
{
  os << "action: " << cs.action << endl;
  os << "<check_suite>" << endl << cs.check_suite;
  os << "<repository>" << endl << cs.repository;
  os << "<installation>" << endl << cs.installation;
  os << endl;

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
installation_access_token::
installation_access_token (json::parser& p)
{
  p.next_expect (event::begin_object);

  // Skip unknown/uninteresting members.
  //
  while (p.next_expect (event::name, event::end_object))
  {
    const string& n (p.name ());

    if (n == "token")
      token = p.next_expect_string ();
    else if (n == "expires_at")
    {
      const string& s (p.next_expect_string ());
      expires_at = from_string (s.c_str (), "%Y-%m-%dT%TZ", false /* local */);
    }
    else p.next_expect_value_skip ();
  }
}

static ostream&
operator<< (ostream& os, const installation_access_token& t)
{
  os << "token: " << t.token << endl;
  os << "expires_at: " << t.expires_at << endl;

  return os;
}
