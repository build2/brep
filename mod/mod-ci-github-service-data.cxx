// file      : mod/mod-ci-github-service-data.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-ci-github-service-data.hxx>

#include <libbutl/json/parser.hxx>
#include <libbutl/json/serializer.hxx>

namespace brep
{
  using event = json::event;

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

    warning_success = p.next_expect_member_boolean<bool> ("warning_success");

    // Installation access token.
    //
    p.next_expect_name ("installation_access");
    installation_access = gh_installation_access_token (p);

    installation_id =
        p.next_expect_member_number<uint64_t> ("installation_id");
    repository_node_id = p.next_expect_member_string ("repository_node_id");
    head_sha = p.next_expect_member_string ("head_sha");

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

      build_state s (to_build_state (p.next_expect_member_string ("state")));
      bool ss (p.next_expect_member_boolean<bool> ("state_synced"));

      check_runs.emplace_back (move (bid), move (nm), move (nid), s, ss);

      p.next_expect (event::end_object);
    }

    p.next_expect (event::end_object);
  }

  service_data::
  service_data (bool ws,
                string iat_tok,
                timestamp iat_ea,
                uint64_t iid,
                string rid,
                string hs)
      : warning_success (ws),
        installation_access (move (iat_tok), iat_ea),
        installation_id (iid),
        repository_node_id (move (rid)),
        head_sha (move (hs))
  {
  }

  string service_data::
  json () const
  {
    string b;
    json::buffer_serializer s (b);

    s.begin_object ();

    s.member ("version", 1);

    s.member ("warning_success", warning_success);

    // Installation access token.
    //
    s.member_begin_object ("installation_access");
    s.member ("token", installation_access.token);
    s.member ("expires_at", gh_to_iso8601 (installation_access.expires_at));
    s.end_object ();

    s.member ("installation_id", installation_id);
    s.member ("repository_node_id", repository_node_id);
    s.member ("head_sha", head_sha);

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

      s.end_object ();
    }
    s.end_array ();

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
