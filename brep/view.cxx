// file      : brep/view.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/view>

#include <memory> // shared_ptr, make_shared()
#include <ostream>

#include <web/module>

using namespace std;

namespace brep
{
  void view::
  init (cli::scanner& s)
  {
    options_ = make_shared<view_options> (s,
                                          cli::unknown_mode::fail,
                                          cli::unknown_mode::fail);
  }

  void view::
  handle (request& rq, response& rs)
  {
    ostream& o (rs.content (200, "text/html;charset=utf-8", false));

    o << "<html><head></head><body>";

    o << "<b>Options:</b>"
      << "<br>\ntracing verbosity: " << options_->verb ()
      << "<br>\ndb endpoint: " << options_->db_host () << ":"
      << options_->db_port ();

    o << "<p>\n<b>Cookies:</b>";

    for (const auto& c: rq.cookies ())
    {
      o << "<br>\n" << c.name << "=" << c.value;
    }

    o << "<p><a href='search?a=1&b&c=2&d=&&x=a+b'>Search</a>"
      << "</body></html>";
  }
}
