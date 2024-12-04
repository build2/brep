// file      : mod/mod-ci-github-gh.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-ci-github-gh.hxx>

#include <libbutl/json/parser.hxx>

namespace brep
{
  [[noreturn]] static void
  throw_json (const json::parser& p, const string& m)
  {
    throw json::invalid_json_input (
      p.input_name,
      p.line (), p.column (), p.position (),
      m);
  }

  // Return the GitHub check run status corresponding to a build_state.
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
    throw_json (p, o + string (" object is missing member '") + m + '\'');
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

      if      (c (ni, "node_id")) node_id = p.next_expect_string ();
      else if (c (hb, "head_branch"))
      {
        string* v (p.next_expect_string_null ());
        if (v != nullptr)
          head_branch = *v;
      }
      else if (c (hs, "head_sha")) head_sha = p.next_expect_string ();
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
       << ", head_branch: " << (cs.head_branch ? *cs.head_branch : "null")
       << ", head_sha: " << cs.head_sha;

    return os;
  }

  // gh_check_suite_ex
  //
  gh_check_suite_ex::
  gh_check_suite_ex (json::parser& p)
  {
    p.next_expect (event::begin_object);

    bool ni (false), hb (false), hs (false), cc (false), co (false),
      ap (false);

    // Skip unknown/uninteresting members.
    //
    while (p.next_expect (event::name, event::end_object))
    {
      auto c = [&p] (bool& v, const char* s)
      {
        return p.name () == s ? (v = true) : false;
      };

      if      (c (ni, "node_id")) node_id = p.next_expect_string ();
      else if (c (hb, "head_branch"))
      {
        string* v (p.next_expect_string_null ());
        if (v != nullptr)
          head_branch = *v;
      }
      else if (c (hs, "head_sha")) head_sha = p.next_expect_string ();
      else if (c (cc, "latest_check_runs_count"))
        check_runs_count = p.next_expect_number <size_t> ();
      else if (c (co, "conclusion"))
      {
        string* v (p.next_expect_string_null ());
        if (v != nullptr)
          conclusion = *v;
      }
      else if (c (ap, "app"))
      {
        p.next_expect (event::begin_object);

        bool ai (false);

        // Skip unknown/uninteresting members.
        //
        while (p.next_expect (event::name, event::end_object))
        {
          if (c (ai, "id"))
          {
            // Note: unlike the check_run webhook's app.id, the check_suite
            // one can be null. It's unclear under what circumstances, but it
            // shouldn't happen unless something is broken.
            //
            string* v (p.next_expect_number_null ());

            if (v == nullptr)
              throw_json (p, "check_suite.app.id is null");

            app_id = *v;
          }
          else p.next_expect_value_skip ();
        }

        if (!ai) missing_member (p, "gh_check_suite_ex.app", "id");
      }
      else p.next_expect_value_skip ();
    }

    if (!ni) missing_member (p, "gh_check_suite_ex", "node_id");
    if (!hb) missing_member (p, "gh_check_suite_ex", "head_branch");
    if (!hs) missing_member (p, "gh_check_suite_ex", "head_sha");
    if (!cc) missing_member (p, "gh_check_suite_ex", "latest_check_runs_count");
    if (!co) missing_member (p, "gh_check_suite_ex", "conclusion");
    if (!ap) missing_member (p, "gh_check_suite_ex", "app");
  }

  ostream&
  operator<< (ostream& os, const gh_check_suite_ex& cs)
  {
    os << "node_id: " << cs.node_id
       << ", head_branch: " << (cs.head_branch ? *cs.head_branch : "null")
       << ", head_sha: " << cs.head_sha
       << ", latest_check_runs_count: " << cs.check_runs_count
       << ", conclusion: " << (cs.conclusion ? *cs.conclusion : "null")
       << ", app_id: " << cs.app_id;

    return os;
  }

  // gh_check_run
  //
  gh_check_run::
  gh_check_run (json::parser& p)
  {
    p.next_expect (event::begin_object);

    bool ni (false), nm (false), st (false);

    // Skip unknown/uninteresting members.
    //
    while (p.next_expect (event::name, event::end_object))
    {
      auto c = [&p] (bool& v, const char* s)
      {
        return p.name () == s ? (v = true) : false;
      };

      if      (c (ni, "node_id")) node_id = p.next_expect_string ();
      else if (c (nm, "name"))    name = p.next_expect_string ();
      else if (c (st, "status"))  status = p.next_expect_string ();
      else p.next_expect_value_skip ();
    }

    if (!ni) missing_member (p, "gh_check_run", "node_id");
    if (!nm) missing_member (p, "gh_check_run", "name");
    if (!st) missing_member (p, "gh_check_run", "status");
  }

  // gh_check_run_ex
  //
  gh_check_run_ex::
  gh_check_run_ex (json::parser& p)
  {
    p.next_expect (event::begin_object);

    bool ni (false), nm (false), st (false), du (false), cs (false),
      ap (false);

    // Skip unknown/uninteresting members.
    //
    while (p.next_expect (event::name, event::end_object))
    {
      auto c = [&p] (bool& v, const char* s)
      {
        return p.name () == s ? (v = true) : false;
      };

      if      (c (ni, "node_id"))     node_id = p.next_expect_string ();
      else if (c (nm, "name"))        name = p.next_expect_string ();
      else if (c (st, "status"))      status = p.next_expect_string ();
      else if (c (du, "details_url")) details_url = p.next_expect_string ();
      else if (c (cs, "check_suite")) check_suite = gh_check_suite (p);
      else if (c (ap, "app"))
      {
        p.next_expect (event::begin_object);

        bool ai (false);

        // Skip unknown/uninteresting members.
        //
        while (p.next_expect (event::name, event::end_object))
        {
          if (c (ai, "id")) app_id = p.next_expect_number ();
          else p.next_expect_value_skip ();
        }

        if (!ai) missing_member (p, "gh_check_run_ex.app", "id");
      }
      else p.next_expect_value_skip ();
    }

    if (!ni) missing_member (p, "gh_check_run_ex", "node_id");
    if (!nm) missing_member (p, "gh_check_run_ex", "name");
    if (!st) missing_member (p, "gh_check_run_ex", "status");
    if (!du) missing_member (p, "gh_check_run_ex", "details_url");
    if (!cs) missing_member (p, "gh_check_run_ex", "check_suite");
    if (!ap) missing_member (p, "gh_check_run_ex", "app");
  }


  ostream&
  operator<< (ostream& os, const gh_check_run& cr)
  {
    os << "node_id: " << cr.node_id
       << ", name: " << cr.name
       << ", status: " << cr.status;

    return os;
  }

  ostream&
  operator<< (ostream& os, const gh_check_run_ex& cr)
  {
    os << static_cast<const gh_check_run&> (cr)
       << ", details_url: " << cr.details_url
       << ", check_suite: { " << cr.check_suite << " }"
       << ", app_id: " << cr.app_id;

    return os;
  }

  gh_pull_request::
  gh_pull_request (json::parser& p)
  {
    p.next_expect (event::begin_object);

    bool ni (false), nu (false), bs (false), hd (false);

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
      else if (c (bs, "base"))
      {
        p.next_expect (event::begin_object);

        bool r (false), s (false), rp (false), fn (false);

        while (p.next_expect (event::name, event::end_object))
        {
          if      (c (r, "ref"))   base_ref = p.next_expect_string ();
          else if (c (s, "sha"))   base_sha = p.next_expect_string ();
          else if (c (rp, "repo"))
          {
            p.next_expect (event::begin_object);

            while (p.next_expect (event::name, event::end_object))
            {
              if (c (fn, "full_name"))
                base_path = p.next_expect_string ();
              else
                p.next_expect_value_skip ();
            }
          }
          else p.next_expect_value_skip ();
        }

        if (!r)  missing_member (p, "gh_pull_request.base", "ref");
        if (!s)  missing_member (p, "gh_pull_request.base", "sha");
        if (!rp) missing_member (p, "gh_pull_request.base", "repo");
        if (!fn) missing_member (p, "gh_pull_request.base.repo", "full_name");
      }
      else if (c (hd, "head"))
      {
        p.next_expect (event::begin_object);

        bool r (false), s (false), rp (false), fn (false);

        while (p.next_expect (event::name, event::end_object))
        {
          if      (c (r, "ref"))   head_ref = p.next_expect_string ();
          else if (c (s, "sha"))   head_sha = p.next_expect_string ();
          else if (c (rp, "repo"))
          {
            p.next_expect (event::begin_object);

            while (p.next_expect (event::name, event::end_object))
            {
              if (c (fn, "full_name"))
                head_path = p.next_expect_string ();
              else
                p.next_expect_value_skip ();
            }
          }
          else p.next_expect_value_skip ();
        }

        if (!r)  missing_member (p, "gh_pull_request.head", "ref");
        if (!s)  missing_member (p, "gh_pull_request.head", "sha");
        if (!rp) missing_member (p, "gh_pull_request.head", "repo");
        if (!fn) missing_member (p, "gh_pull_request.head.repo", "full_name");
      }
      else p.next_expect_value_skip ();
    }

    if (!ni) missing_member (p, "gh_pull_request", "node_id");
    if (!nu) missing_member (p, "gh_pull_request", "number");
    if (!bs) missing_member (p, "gh_pull_request", "base");
    if (!hd) missing_member (p, "gh_pull_request", "head");
  }

  ostream&
  operator<< (ostream& os, const gh_pull_request& pr)
  {
    os << "node_id: " << pr.node_id
       << ", number: " << pr.number
       << ", base: { "
       << "path: " << pr.base_path
       << ", ref: " << pr.base_ref
       << ", sha: " << pr.base_sha
       << " }"
       << ", head: { "
       << "path: " << pr.head_path
       << ", ref: " << pr.head_ref
       << ", sha: " << pr.head_sha
       << " }"
       << ", app_id: " << pr.app_id;

    return os;
  }

  // gh_repository
  //
  gh_repository::
  gh_repository (json::parser& p)
  {
    p.next_expect (event::begin_object);

    bool ni (false), fn (false), cu (false);

    // Skip unknown/uninteresting members.
    //
    while (p.next_expect (event::name, event::end_object))
    {
      auto c = [&p] (bool& v, const char* s)
      {
        return p.name () == s ? (v = true) : false;
      };

      if      (c (ni, "node_id"))        node_id = p.next_expect_string ();
      else if (c (fn, "full_name"))      path = p.next_expect_string ();
      else if (c (cu, "clone_url"))      clone_url = p.next_expect_string ();
      else p.next_expect_value_skip ();
    }

    if (!ni) missing_member (p, "gh_repository", "node_id");
    if (!fn) missing_member (p, "gh_repository", "full_name");
    if (!cu) missing_member (p, "gh_repository", "clone_url");
  }

  ostream&
  operator<< (ostream& os, const gh_repository& rep)
  {
    os << "node_id: " << rep.node_id
       << ", path: " << rep.path
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

      if (c (i, "id")) id = p.next_expect_number ();
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
      else if (c (cs, "check_suite"))  check_suite = gh_check_suite_ex (p);
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

  // gh_check_run_event
  //
  gh_check_run_event::
  gh_check_run_event (json::parser& p)
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
      else if (c (cs, "check_run"))    check_run = gh_check_run_ex (p);
      else if (c (rp, "repository"))   repository = gh_repository (p);
      else if (c (in, "installation")) installation = gh_installation (p);
      else p.next_expect_value_skip ();
    }

    if (!ac) missing_member (p, "gh_check_run_event", "action");
    if (!cs) missing_member (p, "gh_check_run_event", "check_run");
    if (!rp) missing_member (p, "gh_check_run_event", "repository");
    if (!in) missing_member (p, "gh_check_run_event", "installation");
  }

  ostream&
  operator<< (ostream& os, const gh_check_run_event& cr)
  {
    os << "action: " << cr.action;
    os << ", check_run { " << cr.check_run << " }";
    os << ", repository { "  << cr.repository << " }";
    os << ", installation { " << cr.installation << " }";

    return os;
  }

  // gh_pull_request_event
  //
  gh_pull_request_event::
  gh_pull_request_event (json::parser& p)
  {
    p.next_expect (event::begin_object);

    bool ac (false), pr (false), bf (false), rp (false), in (false);

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
      else if (c (bf, "before"))       before = p.next_expect_string ();
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
    os << ", before: " << (pr.before ? *pr.before : "null");
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
      else if (c (ea, "expires_at"))
      {
        string v (p.next_expect_string ());

        try
        {
          expires_at = gh_from_iso8601 (v);
        }
        catch (const invalid_argument& e)
        {
          throw_json (p,
                      "invalid IAT expires_at value '" + v +
                        "': " + e.what ());
        }
        catch (const system_error& e)
        {
          // Translate for simplicity.
          //
          throw_json (p,
                      "unable to convert IAT expires_at value '" + v +
                      "': " + e.what ());
        }
      }
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
    return butl::from_string (s.c_str (),
                              "%Y-%m-%dT%TZ",
                              false /* local */);
  }
}
