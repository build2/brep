// file      : mod/mod-ci-github-service-data.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-ci-github-service-data.hxx>

#include <libbutl/json/parser.hxx>
#include <libbutl/json/serializer.hxx>

namespace brep
{
  using event = json::event;

  [[noreturn]] static void
  throw_json (json::parser& p, const string& m)
  {
    throw json::invalid_json_input (
      p.input_name,
      p.line (), p.column (), p.position (),
      m);
  }

  service_data::
  service_data (const string& json)
  {
    json::parser p (json.data (), json.size (), "service_data");

    p.next_expect (event::begin_object);

    // Throw if the schema version is not supported.
    //
    version = p.next_expect_member_number<uint64_t> ("version");
    if (version != 1)
    {
      throw invalid_argument ("unsupported service_data schema version: " +
                              to_string (version));
    }

    {
      string v (p.next_expect_member_string ("kind"));

      if      (v == "local")  kind = local;
      else if (v == "remote") kind = remote;
      else
        throw_json (p, "invalid service data kind: '" + v + '\'');
    }

    pre_check = p.next_expect_member_boolean<bool> ("pre_check");
    re_request = p.next_expect_member_boolean<bool> ("re_request");

    warning_success = p.next_expect_member_boolean<bool> ("warning_success");

    // Installation access token (IAT).
    //
    p.next_expect_name ("installation_access");
    installation_access = gh_installation_access_token (p);

    app_id = p.next_expect_member_number<uint64_t> ("app_id");
    installation_id = p.next_expect_member_string ("installation_id");

    repository_node_id = p.next_expect_member_string ("repository_node_id");
    repository_clone_url = p.next_expect_member_string ("repository_clone_url");

    {
      string* s (p.next_expect_member_string_null ("pr_node_id"));
      if (s != nullptr)
        pr_node_id = *s;
    }

    pr_number = p.next_expect_member_number_null<uint32_t> ("pr_number");

    check_sha = p.next_expect_member_string ("check_sha");
    report_sha = p.next_expect_member_string ("report_sha");

    p.next_expect_member_array ("check_runs");
    while (p.next_expect (event::begin_object, event::end_array))
    {
      string bid (p.next_expect_member_string ("build_id"));
      string nm (p.next_expect_member_string ("name"));

      optional<string> nid;
      {
        string* v (p.next_expect_member_string_null ("node_id"));
        if (v != nullptr)
          nid = *v;
      }

      build_state s;
      try
      {
        s = to_build_state (p.next_expect_member_string ("state"));
      }
      catch (const invalid_argument& e)
      {
        throw_json (p, e.what ());
      }

      bool ss (p.next_expect_member_boolean<bool> ("state_synced"));

      optional<result_status> rs;
      {
        string* v (p.next_expect_member_string_null ("status"));
        if (v != nullptr)
        {
          try
          {
            rs = bbot::to_result_status (*v);
          }
          catch (const invalid_argument& e)
          {
            throw_json (p, e.what ());
          }
          assert (s == build_state::built);
        }
      }

      check_runs.push_back (
        check_run {move (bid),
                   move (nm),
                   move (nid),
                   s,
                   ss,
                   rs,
                   nullopt, /* details_url */
                   nullopt /* description */});

      p.next_expect (event::end_object);
    }

    completed = p.next_expect_member_boolean<bool> ("completed");

    {
      string* s (p.next_expect_member_string_null ("conclusion_node_id"));
      if (s != nullptr)
        conclusion_node_id = *s;
    }

    p.next_expect (event::end_object);
  }

  // check_suite constructor.
  //
  service_data::
  service_data (bool ws,
                string iat_tok,
                timestamp iat_ea,
                uint64_t aid,
                string iid,
                string rid,
                string rcu,
                kind_type k,
                bool pc,
                bool rr,
                string cs,
                string rs)
      : kind (k), pre_check (pc), re_request (rr),
        warning_success (ws),
        installation_access (move (iat_tok), iat_ea),
        app_id (aid),
        installation_id (move (iid)),
        repository_node_id (move (rid)),
        repository_clone_url (move (rcu)),
        check_sha (move (cs)),
        report_sha (move (rs)),
        completed (false)
  {
  }

  // pull_request constructor.
  //
  service_data::
  service_data (bool ws,
                string iat_tok,
                timestamp iat_ea,
                uint64_t aid,
                string iid,
                string rid,
                string rcu,
                kind_type k,
                bool pc,
                bool rr,
                string cs,
                string rs,
                string pid,
                uint32_t prn)
      : kind (k), pre_check (pc), re_request (rr),
        warning_success (ws),
        installation_access (move (iat_tok), iat_ea),
        app_id (aid),
        installation_id (move (iid)),
        repository_node_id (move (rid)),
        repository_clone_url (move (rcu)),
        pr_node_id (move (pid)),
        pr_number (prn),
        check_sha (move (cs)),
        report_sha (move (rs)),
        completed (false)
  {
  }

  string service_data::
  json () const
  {
    string b;
    json::buffer_serializer s (b);

    s.begin_object ();

    s.member ("version", 1);

    s.member_name ("kind");
    switch (kind)
    {
    case local:  s.value ("local"); break;
    case remote: s.value ("remote"); break;
    }

    s.member ("pre_check", pre_check);
    s.member ("re_request", re_request);

    s.member ("warning_success", warning_success);

    // Installation access token (IAT).
    //
    s.member_begin_object ("installation_access");
    s.member ("token", installation_access.token);

    // IAT expires_at timestamp.
    //
    {
      string v;
      try
      {
        v = gh_to_iso8601 (installation_access.expires_at);
      }
      catch (const system_error& e)
      {
        // Translate for simplicity.
        //
        throw invalid_argument ("unable to convert IAT expires_at value " +
                                to_string (system_clock::to_time_t (
                                  installation_access.expires_at)));
      }
      s.member ("expires_at", move (v));
    }

    s.end_object ();

    s.member ("app_id", app_id);
    s.member ("installation_id", installation_id);
    s.member ("repository_node_id", repository_node_id);
    s.member ("repository_clone_url", repository_clone_url);

    s.member_name ("pr_node_id");
    if (pr_node_id)
      s.value (*pr_node_id);
    else
      s.value (nullptr);

    s.member_name ("pr_number");
    if (pr_number)
      s.value (*pr_number);
    else
      s.value (nullptr);

    s.member ("check_sha", check_sha);
    s.member ("report_sha", report_sha);

    s.member_begin_array ("check_runs");
    for (const check_run& cr: check_runs)
    {
      s.begin_object ();
      s.member ("build_id", cr.build_id);
      s.member ("name", cr.name);

      s.member_name ("node_id");
      if (cr.node_id)
        s.value (*cr.node_id);
      else
        s.value (nullptr);

      s.member ("state", to_string (cr.state));
      s.member ("state_synced", cr.state_synced);

      s.member_name ("status");
      if (cr.status)
      {
        assert (cr.state == build_state::built);
        s.value (to_string (*cr.status)); // Doesn't throw.
      }
      else
        s.value (nullptr);

      s.end_object ();
    }
    s.end_array ();

    s.member ("completed", completed);

    s.member_name ("conclusion_node_id");
    if (conclusion_node_id)
      s.value (*conclusion_node_id);
    else
      s.value (nullptr);

    s.end_object ();

    return b;
  }

  check_run* service_data::
  find_check_run (const string& bid)
  {
    for (check_run& cr: check_runs)
    {
      if (cr.build_id == bid)
        return &cr;
    }
    return nullptr;
  }

  ostream&
  operator<< (ostream& os, const check_run& cr)
  {
    os << "node_id: " << cr.node_id.value_or ("null")
       << ", build_id: " << cr.build_id
       << ", name: " << cr.name
       << ", state: " << cr.state_string ();

    return os;
  }
}
