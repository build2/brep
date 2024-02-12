// file      : mod/mod-ci-github.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-ci-github.hxx>

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

struct check_suite_event
{
  string        action;
  ::check_suite check_suite;
  ::repository  repository;

  explicit
  check_suite_event (json::parser&);

  check_suite_event () = default;
};

static ostream&
operator<< (ostream&, const check_suite&);

static ostream&
operator<< (ostream&, const repository&);

static ostream&
operator<< (ostream&, const check_suite_event&);

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

      try
      {
        // Use the maximum validity period allowed by GitHub (10 minutes).
        // @@ Let's make configurable.
        //
        string jwt (gen_jwt (*options_,
                             options_->ci_github_app_private_key (),
                             to_string (options_->ci_github_app_id ()),
                             chrono::minutes (10)));

        if (jwt.empty ())
          fail << "unable to generate JWT: " << options_->openssl ()
               << " failed";

        cout << "JWT: " << jwt << endl;
      }
      catch (const system_error& e)
      {
        fail << "unable to generate JWT: unable to execute "
             << options_->openssl () << ": " << e.what ();
      }
      catch (const std::exception& e)
      {
        fail << "unable to generate JWT: " << e;
      }

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
    throw invalid_request (400, "malformed JSON in request body");
  }
}

using event = json::event;

// check_suite
//
check_suite::check_suite (json::parser& p)
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
repository::repository (json::parser& p)
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

// check_suite_event
//
check_suite_event::check_suite_event (json::parser& p)
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
    else p.next_expect_value_skip ();
  }
}

static ostream&
operator<< (ostream& os, const check_suite_event& cs)
{
  os << "action: " << cs.action << endl;
  os << "<check_suite>" << endl << cs.check_suite;
  os << "<repository>" << endl << cs.repository << endl;

  return os;
}
