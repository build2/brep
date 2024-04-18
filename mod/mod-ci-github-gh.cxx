// file      : mod/mod-ci-github-gh.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-ci-github-gh.hxx>

#include <libbutl/json/parser.hxx>

namespace brep
{
  static const string gh_status[] {"QUEUED", "IN_PROGRESS", "COMPLETED"};

  // Return the GitHub check run status corresponding to a build_state.
  //
  string
  gh_to_status (build_state st)
  {
    // @@ Just return by value (small string optimization).
    //
    //   @@ TMP Keep this comment, right?
    //
    return gh_status[static_cast<size_t> (st)];
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
      throw invalid_argument ("invalid GitHub check run status: '" + s +
                              '\'');
  }

  string
  gh_check_run_name (const build& b,
                     const tenant_service_base::build_hints* bh)
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

    if (!ni) missing_member (p, "gh_check_suite", "node_id");
    if (!hb) missing_member (p, "gh_check_suite", "head_branch");
    if (!hs) missing_member (p, "gh_check_suite", "head_sha");
    if (!bf) missing_member (p, "gh_check_suite", "before");
    if (!at) missing_member (p, "gh_check_suite", "after");
  }

  ostream&
  operator<< (ostream& os, const gh_check_suite& cs)
  {
    os << "node_id: " << cs.node_id
       << ", head_branch: " << cs.head_branch
       << ", head_sha: " << cs.head_sha
       << ", before: " << cs.before
       << ", after: " << cs.after;

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
      else if (c (ea, "expires_at")) expires_at = from_iso8601 (p.next_expect_string ());
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
  to_iso8601 (timestamp t)
  {
    return butl::to_string (t,
                            "%Y-%m-%dT%TZ",
                            false /* special */,
                            false /* local */);
  }

  timestamp
  from_iso8601 (const string& s)
  {
    return butl::from_string (s.c_str (), "%Y-%m-%dT%TZ", false /* local */);
  }
}
