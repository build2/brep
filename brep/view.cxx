// file      : brep/view.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/view>

#include <memory> // shared_ptr, make_shared()
#include <ostream>

#include <odb/session.hxx>
#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <web/module>

#include <brep/package>
#include <brep/package-odb>
#include <brep/shared-database>

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

    db_ = shared_database (options_->db_host (), options_->db_port ());
  }

  void view::
  handle (request& rq, response& rs)
  {
    session s;
    transaction t (db_->begin ());

    shared_ptr<package> p (db_->load<package> ("cli"));

    for (auto& vp: p->versions)
    {
      shared_ptr<package_version> v (vp.load ());
      v->repository.load ();
      v->package.load ();
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

    for (const auto& vp: p->versions)
    {
      // Just finds package_version object in session cache.
      //
      shared_ptr<package_version> v (vp.load ());

      assert (v != nullptr);
      assert (v->repository.get_eager () != nullptr);
      assert (v->package.get_eager () != nullptr);

      o << "<br>version:" << v->version.string ()
        << "<br>package:" << v->package->name
        << "<br>repo:" << v->repository->display_name
        << "<br>changes:" << v->changes
        << "<br>licenses:" << v->license_alternatives.size ();

      for (const auto& la: v->license_alternatives)
      {
        o << "<br>";

        for (const auto& l: la)
        {
          o << " |" << l << "|";
        }
      }

      o << "<br>deps:" << v->dependencies.size ();

      for (const auto& da: v->dependencies)
      {
        o << "<br>";

        for (const auto& d: da)
        {
          o << " |" << d.package;

          if (d.version)
          {
            o << "," << d.version->value.string () << ","
              << static_cast<int> (d.version->operation) << "|";
          }
        }
      }

      o << "<br>requirements:" << v->requirements.size ();

      for (const auto& ra: v->requirements)
      {
        o << "<br>";

        for (const auto& r: ra)
        {
          o << " |" << r << "|";
        }
      }
    }

    o << "<p><a href='search?a=1&b&c=2&d=&&x=a+b'>Search</a>"
      << "</body></html>";
  }
}
