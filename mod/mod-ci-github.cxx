// file      : mod/mod-ci-github.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-ci-github.hxx>

#include <libbutl/json/parser.hxx>

#include <mod/module-options.hxx>

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
//      The inputs are (one of) the application's private key(s) and the
//      application ID, which goes into the "issuer" JWT field. Also the
//      token's issued time and expiration time (10 minutes max).
//
//      The best library for us seems to be libjwt at
//      https://github.com/benmcollins/libjwt which is also widely packaged
//      (most Linux distros and Homebrew).
//
//      Doing it ourselves:
//
//      Github requires the RS256 algorithm, which is RSA signing using
//      SHA256.
//
//      The message consists of a base64url-encoded JSON header and
//      payload. (base64url differs from base64 by having different 62nd and
//      63rd alphabet characters (- and _ instead of ~ and . to make it
//      filesystem-safe) and no padding because the padding character '=' is
//      unsafe in URIs.)
//
//      Header:
//
//      {
//        "alg": "RS256",
//        "typ": "JWT"
//      }
//
//      Payload:
//
//      {
//        "iat": 1234567,
//        "exp": 1234577,
//        "iss": "<APP_ID>"
//      }
//
//      Where:
//      iat := Issued At (NumericDate)
//      exp := Expires At (NumericDate)
//      iss := Issuer
//
//      Signature:
//
//        RSA_SHA256(PKEY, base64url($header) + "." + base64url($payload))
//
//      JWT:
//
//        base64url($header) + "." +
//        base64url($payload) + "." +
//        base64url($signature)
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

using namespace std;
using namespace butl;
using namespace web;
using namespace brep::cli;

brep::ci_github::ci_github (const ci_github& r)
    : handler (r),
      options_ (r.initialized_ ? r.options_ : nullptr)
{
}

void brep::ci_github::
init (scanner& s)
{
  options_ = make_shared<options::ci> (
    s, unknown_mode::fail, unknown_mode::fail);
}

bool brep::ci_github::
respond (response& rs, status_code status, const string& message)
{
  ostream& os (rs.content (status, "text/plain;charset=utf-8"));

  os << message;

  return true;
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
};

struct check_suite_event
{
  string        action;
  ::check_suite check_suite;
  ::repository  repository;
};

bool brep::ci_github::
handle (request& rq, response& rs)
{
  using namespace bpkg;

  HANDLER_DIAG;

  // @@ TODO
  if (false)
    return respond (rs, 404, "CI request submission disabled");

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
          return respond (rs, 400, "missing content-type value");

        if (icasecmp (*h.value, "application/json") != 0)
          return respond (rs, 400,
                          "invalid content-type value: '" + *h.value + '\'');
      }
      else if (icasecmp (h.name, "x-github-event") == 0)
      {
        if (!h.value)
          return respond (rs, 400, "missing x-github-event value");

        event = *h.value;
      }
    }

    if (!content_type)
      return respond (rs, 400, "missing content-type header");

    if (event.empty ())
      return respond (rs, 400, "missing x-github-event header");
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

      p.next_expect (event::begin_object); // @@ Let's move into ctor (everywhere).
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
        return respond (rs, 400, "unsupported action: " + cs.action);

      cout << "<check_suite webhook>" << endl << cs << endl;

      return true;
    }
    else if (event == "pull_request")
    {
      return respond (rs, 501, "pull request events not implemented yet");
    }
    else
      respond (rs, 400, "unexpected event: '" + event + "'");
  }
  catch (const json::invalid_json_input& e)
  {
    // @@ TODO: should we write more detailed diagnostics to log? Maybe we
    //    should do this for all unsuccessful calls to respond().

    return respond (rs, 400, "malformed JSON in request body");
  }
}

using event = json::event;

// check_suite_obj
//

// @@ Let's make then constructors.
//
static check_suite_obj
parse_check_suite_obj (json::parser& p)
{
  check_suite_obj r;

  // Skip unknown/uninteresting members.
  //
  while (p.next_expect (event::name, event::end_object))
  {
    const string& n (p.name ());

    if      (n == "id")          r.id = p.next_expect_number<uint64_t> ();
    else if (n == "head_branch") r.head_branch = p.next_expect_string ();
    else if (n == "head_sha")    r.head_sha = p.next_expect_string ();
    else if (n == "before")      r.before = p.next_expect_string ();
    else if (n == "after")       r.after = p.next_expect_string ();
    else p.next_expect_value_skip ();
  }

  return r;
}

static ostream&
operator<< (ostream& os, const check_suite_obj& cs)
{
  os << "id: " << cs.id << endl
     << "head_branch: " << cs.head_branch << endl
     << "head_sha: " << cs.head_sha << endl
     << "before: " << cs.before << endl
     << "after: " << cs.after << endl;

  return os;
}

// repository_obj
//
static repository_obj
parse_repository_obj (json::parser& p)
{
  repository_obj r;

  // Skip unknown/uninteresting members.
  //
  while (p.next_expect (event::name, event::end_object))
  {
    const string& n (p.name ());

    if      (n == "name")           r.name = p.next_expect_string ();
    else if (n == "full_name")      r.full_name = p.next_expect_string ();
    else if (n == "default_branch") r.default_branch = p.next_expect_string ();
    else p.next_expect_value_skip ();
  }

  return r;
}

static ostream&
operator<< (ostream& os, const repository_obj& rep)
{
  os << "name: " << rep.name << endl
     << "full_name: " << rep.full_name << endl
     << "default_branch: " << rep.default_branch << endl;

  return os;
}

// check_suite_req
//
static check_suite_req
parse_check_suite_webhook (json::parser& p)
{
  check_suite_req r;

  // @@ Let's not assume order.

  r.action = p.next_expect_member_string ("action");

  // Parse the check_suite object.
  //
  p.next_expect_name ("check_suite");
  p.next_expect (event::begin_object);
  r.check_suite = parse_check_suite_obj (p);

  // Parse the repository object.
  //
  p.next_expect_name ("repository", true /* skip_unknown */);
  p.next_expect (event::begin_object);
  r.repository = parse_repository_obj (p);

  return r;
}

static ostream&
operator<< (ostream& os, const check_suite_req& cs)
{
  os << "action: " << cs.action << endl;
  os << "<check_suite>" << endl << cs.check_suite;
  os << "<repository>" << endl << cs.repository << endl;

  return os;
}
