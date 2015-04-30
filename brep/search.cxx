// file      : brep/search.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <brep/search>

#include <memory> // shared_ptr, make_shared()
#include <chrono>
#include <ostream>

#include <web/module>

using namespace std;

namespace brep
{
  void search::
  init (::cli::scanner& s)
  {
    MODULE_DIAG;

    options_ = std::make_shared<search_options> (s,
                                                 ::cli::unknown_mode::fail,
                                                 ::cli::unknown_mode::fail);

    if (options_->results_on_page () > 30)
      fail << "too many search results on page: "
           << options_->results_on_page ();
    else if (options_->results_on_page () > 10)
      warn << options_->results_on_page ()
           << " search results on page is quite a lot but will try to cope";
  }

  void search::
  handle (request& rq, response& rs)
  {
    MODULE_DIAG;

    chrono::seconds ma (60);
    rs.cookie ("Oh", " Ah\n\n", &ma, "/");
    rs.cookie ("Hm", ";Yes", &ma);

    info << "handling search request from "; // << rq.client_ip ();

    ostream& o (rs.content ());

    o << "<html><head></head><body>";

    o << "<b>Options:</b>"
      << "<br>\ntracing verbosity: " << options_->verb ()
      << "<br>\ndb endpoint: " << options_->db_host () << ":"
      << options_->db_port ()
      << "<br>\nsearch results on page: " << options_->results_on_page ();

    o << "<p>\n<b>Params:</b>";

    const name_values& ps (rq.parameters ());

    if (ps.empty ())
      throw invalid_request (422, "search parameters expected");

    if (ps.size () > 100)
      fail << "too many parameters: " << ps.size () <<
        info << "are you crazy to specify so many?";

    level2 ([&]{trace << "search request with " << ps.size () << " params";});

    for (const auto& p: ps)
    {
      o << "<br>\n" << p.name << "=" << p.value;
    }

    o << "<p>\n<b>Cookies:</b>";

    for (const auto& c: rq.cookies ())
    {
      o << "<br>\n" << c.name << "=" << c.value;
    }

    o << "<p><a href='view'>View</a>"
      << "</body></html>";
  }
}
