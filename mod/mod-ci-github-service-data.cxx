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

    // Installation access token.
    //
    p.next_expect_member_object ("installation_access");
    installation_access.token = p.next_expect_member_string ("token");
    installation_access.expires_at =
        gh_from_iso8601 (p.next_expect_member_string ("expires_at"));
    p.next_expect (event::end_object);

    installation_id =
        p.next_expect_member_number<uint64_t> ("installation_id");
    repository_id = p.next_expect_member_string ("repository_id");
    head_sha = p.next_expect_member_string ("head_sha");

    p.next_expect_member_array ("check_runs");
    while (p.next_expect (event::begin_object, event::end_array))
    {
      string bid (p.next_expect_member_string ("build_id"));

      optional<string> nid;
      {
        string* v (p.next_expect_member_string_null ("node_id"));
        if (v != nullptr)
          nid = *v;
      }

      optional<build_state> s;
      {
        string* v (p.next_expect_member_string_null ("state"));
        if (v != nullptr)
          s = to_build_state (*v);
      }

      check_runs.emplace_back (move (bid), move (nid), s);

      p.next_expect (event::end_object);
    }

    p.next_expect (event::end_object);
  }

  service_data::
  service_data (string iat_tok,
                timestamp iat_ea,
                uint64_t iid,
                string rid,
                string hs)
      : installation_access (move (iat_tok), iat_ea),
        installation_id (iid),
        repository_id (move (rid)),
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

    // Installation access token.
    //
    s.member_begin_object ("installation_access");
    s.member ("token", installation_access.token);
    s.member ("expires_at", gh_to_iso8601 (installation_access.expires_at));
    s.end_object ();

    s.member ("installation_id", installation_id);
    s.member ("repository_id", repository_id);
    s.member ("head_sha", head_sha);

    s.member_begin_array ("check_runs");
    for (const check_run& cr: check_runs)
    {
      s.begin_object ();
      s.member ("build_id", cr.build_id);

      s.member_name ("node_id");
      if (cr.node_id)
        s.value (*cr.node_id);
      else
        s.value (nullptr);

      s.member_name ("state");
      if (cr.state)
        s.value (to_string (*cr.state));
      else
        s.value (nullptr);

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
       << ", state: " << (cr.state ? to_string (*cr.state) : "null");

    return os;
  }
}
