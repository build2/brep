// file      : brep/search.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/search>

#include <memory> // shared_ptr, make_shared()
#include <chrono>
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

namespace brep
{
  void search::
  init (cli::scanner& s)
  {
    MODULE_DIAG;

    options_ = make_shared<search_options> (s,
                                            cli::unknown_mode::fail,
                                            cli::unknown_mode::fail);

    db_ = make_shared<odb::pgsql::database>("",
                                            "",
                                            "brep",
                                            options_->db_host (),
                                            options_->db_port ());

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

    std::shared_ptr<package> cli (make_shared<package> ());

    cli->name = "cli";
    cli->summary = "CLI is ...";
    cli->description = "This is CLI";
    cli->tags.push_back ("compiler");
    cli->tags.push_back ("C++");

    licenses l;
    l.comment = "License\"A'";
    l.push_back ("XXX");
    l.push_back ("AAA");
    l.push_back ("BBB");
    l.push_back ("CCC");

    std::shared_ptr<package_version> v (make_shared<package_version> ());

    v->version = version ("1.1");
    v->package = cli;

    v->license_alternatives.push_back (l);

    dependency_alternatives da;
    da.push_back (
      {"icl", version_comparison{version ("1.3.3"),  comparison::gt}});

    da.push_back (
      {"ocl", version_comparison{version ("1.5.5"),  comparison::lt}});

    v->dependencies.push_back (da);

    {
      requirement_alternatives ra;
      ra.push_back ("TAO");
      ra.push_back ("ORBacus");

      v->requirements.push_back (ra);
    }

    {
      requirement_alternatives ra;
      ra.push_back ("Xerces");

      v->requirements.push_back (ra);
    }

    cli->versions.push_back (v);

    transaction t (db_->begin ());
//    t.tracer (odb::stderr_full_tracer);

    {
      db_->persist (cli);
      db_->persist (v);
    }

    t.commit ();

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
