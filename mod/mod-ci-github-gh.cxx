// file      : mod/mod-ci-github-gh.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-ci-github-gh.hxx>

#include <libbutl/json/parser.hxx>

namespace brep
{
  // Return the GitHub check run status corresponding to a build_state. Throw
  // invalid_argument if the build_state value was invalid.
  //
  string
  gh_to_status (build_state st)
  {
    // Just return by value (small string optimization).
    //
    switch (st)
    {
    case build_state::queued:   return "QUEUED";
    case build_state::building: return "IN_PROGRESS";
    case build_state::built:    return "COMPLETED";
    }

    return ""; // Should never reach.
  }

  // Return the build_state corresponding to a GitHub check run status
  // string. Throw invalid_argument if the passed status was invalid.
  //
  build_state
  gh_from_status (const string& s)
  {
    if      (s == "QUEUED")      return build_state::queued;
    else if (s == "IN_PROGRESS") return build_state::building;
    else if (s == "COMPLETED")   return build_state::built;
    else
      throw invalid_argument ("unexpected GitHub check run status: '" + s +
                              '\'');
  }

  string
  gh_to_conclusion (result_status rs, bool warning_success)
  {
    switch (rs)
    {
    case result_status::success:
      return "SUCCESS";

    case result_status::warning:
      return warning_success ? "SUCCESS" : "FAILURE";

    case result_status::error:
    case result_status::abort:
    case result_status::abnormal:
      return "FAILURE";

      // Valid values we should never encounter.
      //
    case result_status::skip:
    case result_status::interrupt:
      throw invalid_argument ("unexpected result_status value: " +
                              to_string (rs));
    }

    return ""; // Should never reach.
  }

  string
  gh_check_run_name (const build& b, const build_queued_hints* bh)
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

  using event = json::event;

  // gh_check_suite
  //
  gh_check_suite::
  gh_check_suite (json::parser& p)
  {
    p.next_expect (event::begin_object);

    bool ni (false), hb (false), hs (false);

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
      else p.next_expect_value_skip ();
    }

    if (!ni) missing_member (p, "gh_check_suite", "node_id");
    if (!hb) missing_member (p, "gh_check_suite", "head_branch");
    if (!hs) missing_member (p, "gh_check_suite", "head_sha");
  }

  ostream&
  operator<< (ostream& os, const gh_check_suite& cs)
  {
    os << "node_id: " << cs.node_id
       << ", head_branch: " << cs.head_branch
       << ", head_sha: " << cs.head_sha;

    return os;
  }

  // gh_check_run
  //
  gh_check_run::
  gh_check_run (json::parser& p)
  {
    p.next_expect (event::begin_object);

    // We always ask for this exact set of fields to be returned in GraphQL
    // requests.
    //
    node_id = p.next_expect_member_string ("id");
    name = p.next_expect_member_string ("name");
    status = p.next_expect_member_string ("status");

    p.next_expect (event::end_object);
  }

  ostream&
  operator<< (ostream& os, const gh_check_run& cr)
  {
    os << "node_id: " << cr.node_id
       << ", name: " << cr.name
       << ", status: " << cr.status;

    return os;
  }

  gh_pull_request::
  gh_pull_request (json::parser& p)
  {
    p.next_expect (event::begin_object);

    bool ni (false), nu (false), st (false), ma (false), ms (false),
      bs (false), hd (false);

    // Skip unknown/uninteresting members.
    //
    while (p.next_expect (event::name, event::end_object))
    {
      auto c = [&p] (bool& v, const char* s)
      {
        return p.name () == s ? (v = true) : false;
      };

      if      (c (ni, "node_id"))   node_id = p.next_expect_string ();
      else if (c (nu, "number"))    number = p.next_expect_number<unsigned int> ();
      else if (c (st, "state"))     state = p.next_expect_string ();
      else if (c (ma, "mergeable")) mergeable = p.next_expect_boolean_null<bool> ();
      else if (c (ms, "merge_commit_sha"))
      {
        string* v (p.next_expect_string_null ());
        if (v != nullptr)
          merge_commit_sha = *v;
      }
      else if (c (bs, "base"))
      {
        p.next_expect (event::begin_object);

        bool l (false), r (false), s (false);

        while (p.next_expect (event::name, event::end_object))
        {
          if      (c (l, "label")) base_label = p.next_expect_string ();
          else if (c (r, "ref"))   base_ref = p.next_expect_string ();
          else if (c (s, "sha"))   base_sha = p.next_expect_string ();
          else p.next_expect_value_skip ();
        }

        if (!l) missing_member (p, "gh_pull_request.base", "label");
        if (!r) missing_member (p, "gh_pull_request.base", "ref");
        if (!s) missing_member (p, "gh_pull_request.base", "sha");
      }
      else if (c (hd, "head"))
      {
        p.next_expect (event::begin_object);

        bool l (false), r (false), s (false);

        while (p.next_expect (event::name, event::end_object))
        {
          if      (c (l, "label")) head_label = p.next_expect_string ();
          else if (c (r, "ref"))   head_ref = p.next_expect_string ();
          else if (c (s, "sha"))   head_sha = p.next_expect_string ();
          else p.next_expect_value_skip ();
        }

        if (!l) missing_member (p, "gh_pull_request.head", "label");
        if (!r) missing_member (p, "gh_pull_request.head", "ref");
        if (!s) missing_member (p, "gh_pull_request.head", "sha");
      }
      else p.next_expect_value_skip ();
    }

    if (!ni) missing_member (p, "gh_pull_request", "node_id");
    if (!nu) missing_member (p, "gh_pull_request", "number");
    if (!st) missing_member (p, "gh_pull_request", "state");
    if (!ma) missing_member (p, "gh_pull_request", "mergeable");
    if (!ms) missing_member (p, "gh_pull_request", "merge_commit_sha");
    if (!bs) missing_member (p, "gh_pull_request", "base");
    if (!hd) missing_member (p, "gh_pull_request", "head");
  }

  ostream&
  operator<< (ostream& os, const gh_pull_request& pr)
  {
    os << "node_id: " << pr.node_id
       << ", number: " << pr.number
       << ", state: " << pr.state
       << ", mergeable: " << (pr.mergeable
                              ? *pr.mergeable
                                ? "true"
                                : "false"
                              : "null")
       << ", merge_commit_sha:" << pr.merge_commit_sha
       << ", base: { "
       << "label: " << pr.base_label
       << ", ref: " << pr.base_ref
       << ", sha: " << pr.base_sha
       << " }"
       << ", head: { "
       << "label: " << pr.head_label
       << ", ref: " << pr.head_ref
       << ", sha: " << pr.head_sha
       << " }";

    return os;
  }

  // gh_repository
  //
  gh_repository::
  gh_repository (json::parser& p)
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

    if (!ni) missing_member (p, "gh_repository", "node_id");
    if (!nm) missing_member (p, "gh_repository", "name");
    if (!fn) missing_member (p, "gh_repository", "full_name");
    if (!db) missing_member (p, "gh_repository", "default_branch");
    if (!cu) missing_member (p, "gh_repository", "clone_url");
  }

  ostream&
  operator<< (ostream& os, const gh_repository& rep)
  {
    os << "node_id: " << rep.node_id
       << ", name: " << rep.name
       << ", full_name: " << rep.full_name
       << ", default_branch: " << rep.default_branch
       << ", clone_url: " << rep.clone_url;

    return os;
  }

  // gh_installation
  //
  gh_installation::
  gh_installation (json::parser& p)
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

    if (!i) missing_member (p, "gh_installation", "id");
  }

  ostream&
  operator<< (ostream& os, const gh_installation& i)
  {
    os << "id: " << i.id;

    return os;
  }

  // gh_check_suite_event
  //
  gh_check_suite_event::
  gh_check_suite_event (json::parser& p)
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
      else if (c (cs, "check_suite"))  check_suite = gh_check_suite (p);
      else if (c (rp, "repository"))   repository = gh_repository (p);
      else if (c (in, "installation")) installation = gh_installation (p);
      else p.next_expect_value_skip ();
    }

    if (!ac) missing_member (p, "gh_check_suite_event", "action");
    if (!cs) missing_member (p, "gh_check_suite_event", "check_suite");
    if (!rp) missing_member (p, "gh_check_suite_event", "repository");
    if (!in) missing_member (p, "gh_check_suite_event", "installation");
  }

  ostream&
  operator<< (ostream& os, const gh_check_suite_event& cs)
  {
    os << "action: " << cs.action;
    os << ", check_suite { " << cs.check_suite << " }";
    os << ", repository { "  << cs.repository << " }";
    os << ", installation { " << cs.installation << " }";

    return os;
  }

  // gh_pull_request_event
  //
  gh_pull_request_event::
  gh_pull_request_event (json::parser& p)
  {
    p.next_expect (event::begin_object);

    bool ac (false), pr (false), rp (false), in (false);

    // Skip unknown/uninteresting members.
    //
    while (p.next_expect (event::name, event::end_object))
    {
      auto c = [&p] (bool& v, const char* s)
      {
        return p.name () == s ? (v = true) : false;
      };

      if      (c (ac, "action"))       action = p.next_expect_string ();
      else if (c (pr, "pull_request")) pull_request = gh_pull_request (p);
      else if (c (rp, "repository"))   repository = gh_repository (p);
      else if (c (in, "installation")) installation = gh_installation (p);
      else p.next_expect_value_skip ();
    }

    if (!ac) missing_member (p, "gh_pull_request_event", "action");
    if (!pr) missing_member (p, "gh_pull_request_event", "pull_request");
    if (!rp) missing_member (p, "gh_pull_request_event", "repository");
    if (!in) missing_member (p, "gh_pull_request_event", "installation");
  }

  ostream&
  operator<< (ostream& os, const gh_pull_request_event& pr)
  {
    os << "action: " << pr.action;
    os << ", pull_request { " << pr.pull_request << " }";
    os << ", repository { "  << pr.repository << " }";
    os << ", installation { " << pr.installation << " }";

    return os;
  }

  // gh_installation_access_token
  //
  // Example JSON:
  //
  // {
  //   "token": "ghs_Py7TPcsmsITeVCAWeVtD8RQs8eSos71O5Nzp",
  //   "expires_at": "2024-02-15T16:16:38Z",
  //   ...
  // }
  //
  gh_installation_access_token::
  gh_installation_access_token (json::parser& p)
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
      else if (c (ea, "expires_at")) expires_at = gh_from_iso8601 (p.next_expect_string ());
      else p.next_expect_value_skip ();
    }

    if (!tk) missing_member (p, "gh_installation_access_token", "token");
    if (!ea) missing_member (p, "gh_installation_access_token", "expires_at");
  }

  gh_installation_access_token::
  gh_installation_access_token (string tk, timestamp ea)
      : token (move (tk)), expires_at (ea)
  {
  }

  ostream&
  operator<< (ostream& os, const gh_installation_access_token& t)
  {
    os << "token: " << t.token << ", expires_at: ";
    butl::operator<< (os, t.expires_at);

    return os;
  }

  string
  gh_to_iso8601 (timestamp t)
  {
    return butl::to_string (t,
                            "%Y-%m-%dT%TZ",
                            false /* special */,
                            false /* local */);
  }

  timestamp
  gh_from_iso8601 (const string& s)
  {
    return butl::from_string (s.c_str (), "%Y-%m-%dT%TZ", false /* local */);
  }
}
