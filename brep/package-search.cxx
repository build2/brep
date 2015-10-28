// file      : brep/package-search.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/package-search>

#include <string>
#include <memory>  // make_shared()
#include <cstddef> // size_t

#include <xml/serializer>

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <web/xhtml>
#include <web/module>
#include <web/mime-url-encoding>

#include <brep/page>
#include <brep/options>
#include <brep/package>
#include <brep/package-odb>
#include <brep/shared-database>

using namespace std;
using namespace cli;
using namespace odb::core;

namespace brep
{
  void package_search::
  init (scanner& s)
  {
    MODULE_DIAG;

    options_ = make_shared<options::package_search> (
      s, unknown_mode::fail, unknown_mode::fail);

    db_ = shared_database (options_->db_host (), options_->db_port ());
  }

  template <typename T>
  static inline query<T>
  search_param (const string& q)
  {
    using query = query<T>;
    return "(" +
      (q.empty ()
       ? query ("NULL")
       : "plainto_tsquery (" + query::_val (q) + ")") +
      ")";
  }

  void package_search::
  handle (request& rq, response& rs)
  {
    using namespace xml;
    using namespace web;
    using namespace web::xhtml;

    MODULE_DIAG;

    params::package_search pr;

    try
    {
      param_scanner s (rq.parameters ());
      pr = params::package_search (s, unknown_mode::fail, unknown_mode::fail);
    }
    catch (const unknown_argument& e)
    {
      throw invalid_request (400, e.what ());
    }

    const string& sq (pr.query ()); // Search query.
    size_t pg (pr.page ());
    string qp (sq.empty () ? "" : "q=" + mime_url_encode (sq));
    size_t rp (options_->results_on_page ());

    serializer s (rs.content (), "Packages");

    const string& title (
      sq.empty () ? s.output_name () : s.output_name () + " " + sq);

    s << HTML
      <<   HEAD
      <<     TITLE << title << ~TITLE
      <<     CSS_LINKS ("/package-search.css")
      <<   ~HEAD
      <<   BODY
      <<     DIV_HEADER ()
      <<     DIV(ID="content");

    transaction t (db_->begin ());

    auto pc (
      db_->query_value<latest_package_count> (
        search_param<latest_package_count> (sq)));

    auto r (
      db_->query<latest_package_search_rank> (
        search_param<latest_package_search_rank> (sq) +
        "ORDER BY rank DESC, name" +
        "OFFSET" + to_string (pg * rp) +
        "LIMIT" + to_string (rp)));

    s << FORM_SEARCH (sq.c_str ())
      << DIV_COUNTER (pc, "Package", "Packages")

      // Enclose the subsequent tables to be able to use nth-child CSS selector.
      //
      <<   DIV;

    for (const auto& pr: r)
    {
      shared_ptr<package> p (db_->load<package> (pr.id));

      s << TABLE(CLASS="proplist package")
        <<   TBODY
        <<     TR_NAME (p->id.name, qp)
        <<     TR_SUMMARY (p->summary)
        <<     TR_LICENSE (p->license_alternatives)
        <<     TR_TAGS (p->tags)
        <<     TR_DEPENDS (p->dependencies)
        <<     TR_REQUIRES (p->requirements)
        <<   ~TBODY
        << ~TABLE;
    }

    t.commit ();

    string url (qp.empty () ? "/" : ("/?" + qp));

    s <<       ~DIV
      <<       DIV_PAGER (pg, pc, rp, options_->pages_in_pager (), url)
      <<     ~DIV
      <<   ~BODY
      << ~HTML;
  }
}
