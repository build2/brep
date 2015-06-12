// file      : brep/view.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/view>

#include <memory> // shared_ptr, make_shared()
#include <ostream>

#include <odb/session.hxx>
#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <odb/pgsql/database.hxx>

#include <web/module>

#include <brep/package>
#include <brep/package-odb.hxx>

using namespace std;
using namespace odb::core;

#pragma db namespace session
namespace brep
{
  void view::
  init (cli::scanner& s)
  {
    options_ = make_shared<view_options> (s,
                                          cli::unknown_mode::fail,
                                          cli::unknown_mode::fail);

    db_ = make_shared<odb::pgsql::database>("",
                                            "",
                                            "brep",
                                            options_->db_host (),
                                            options_->db_port ());
  }

  void view::
  handle (request& rq, response& rs)
  {
    session s;
    transaction t (db_->begin ());

    shared_ptr<package> p (db_->load<package> ("cli"));

    for (auto& vp : p->versions)
    {
      s.cache_insert<package_version> (*db_, vp.object_id (), vp.load ());
    }

    t.commit ();

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

    o << "<p>\n" << p->name << ": " << p->versions.size ();

    for (const auto& vp : p->versions)
    {
      shared_ptr<package_version> v (
        s.cache_find<package_version> (*db_, vp.object_id ()));

      if (!v)
      {
        o << "<br>no version in cache !";
      }
      else
      {
        o << "<br>licenses:" << v->license_alternatives.size ();

        for (const auto& la : v->license_alternatives)
        {
          o << "<br>";

          for (const auto& l : la)
          {
            o << " |" << l << "|";
          }
        }

        o << "<br>deps:" << v->dependencies.size ();

        for (const auto& da : v->dependencies)
        {
          o << "<br>";

          for (const auto& d : da)
          {
            o << " |" << d.package;

            if (!d.version.null ())
            {
              o << "," << d.version->value << ","
                << static_cast<int> (d.version->operation) << "|";
            }
          }
        }

        o << "<br>requirements:" << v->requirements.size ();

        for (const auto& ra : v->requirements)
        {
          o << "<br>";

          for (const auto& r : ra)
          {
            o << " |" << r << "|";
          }
        }
      }

    }

    o << "<p><a href='search?a=1&b&c=2&d=&&x=a+b'>Search</a>"
      << "</body></html>";
  }
}
