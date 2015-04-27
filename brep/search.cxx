// file      : brep/search.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <brep/search>

#include <chrono>
#include <ostream>

#include <web/module>

using namespace std;

namespace brep
{
  void search::
  handle (request& rq, response& rs)
  {
    MODULE_DIAG;

    chrono::seconds ma (60);
    rs.cookie ("Oh", " Ah\n\n", &ma, "/");
    rs.cookie ("Hm", ";Yes", &ma);

    info << "handling search request from "; // << rq.client_ip ();

    ostream& o (rs.content (200, "text/html;charset=utf-8", true));

    o << "<html><head></head><body><b>Params:</b>";

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

    o << "<br>\n<b>Cookies:</b>";

    for (const auto& c: rq.cookies ())
    {
      o << "<br>\n" << c.name << "=" << c.value << " ";
    }

    o << "</body></html>";
  }
}
