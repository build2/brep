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

    // @@ Would be nice to have a manipulator indenting string properly
    //    according to the most nested element identation.
    //
    const char* ident ("\n      ");
    const char* title ("Package Search");
    serializer s (rs.content (), title);

    s << HTML
      <<   HEAD
      <<     TITLE << title << ~TITLE
      <<     CSS_STYLE << ident
      <<       A_STYLE () << ident
      <<       DIV_PAGER_STYLE () << ident
      <<       "#packages {font-size: x-large;}" << ident
      <<       ".package {margin: 0.5em 0 0;}" << ident
      <<       ".name {font-size: x-large;}" << ident
      <<       ".tags {margin: 0.3em 0 0;}" << ident
      <<       "form {margin:  0.5em 0 0 0;}"
      <<     ~CSS_STYLE
      <<   ~HEAD
      <<   BODY;

    const string& sq (pr.query ()); // Search query.
    string qp (sq.empty () ? "" : "q=" + mime_url_encode (sq));
    size_t rop (options_->results_on_page ());

    transaction t (db_->begin ());

    size_t pc (
      db_->query_value<latest_package_count> (
        search_param<latest_package_count> (sq)));

    s << DIV(ID="packages") << "Packages (" << pc << ")" << ~DIV
      << FORM_SEARCH (sq);

    auto r (
      db_->query<latest_package_search_rank> (
        search_param<latest_package_search_rank> (sq) +
        "ORDER BY rank DESC, name" +
        "OFFSET" + to_string (pr.page () * rop) +
        "LIMIT" + to_string (rop)));

    for (const auto& pr: r)
    {
      shared_ptr<package> p (db_->load<package> (pr.id));

      s << DIV(CLASS="package")
        <<   DIV(CLASS="name")
        <<     A
        <<     HREF << "/go/" << mime_url_encode (p->id.name);

      // Propagate search criteria to the package version search url.
      //
      if (!qp.empty ())
        s << "?" << qp;

      s <<     ~HREF
        <<       p->id.name
        <<     ~A
        <<   ~DIV
        <<   DIV(CLASS="summary") << p->summary << ~DIV
        <<   DIV_TAGS (p->tags)
        <<   DIV_LICENSES (p->license_alternatives)
        <<   DIV(CLASS="dependencies")
        <<     "Dependencies: " << p->dependencies.size ()
        <<   ~DIV
        << ~DIV;
    }

    t.commit ();

    string u (qp.empty () ? "/" : ("/?" + qp));

    s <<      DIV_PAGER (pr.page (), pc, rop, options_->pages_in_pager (), u)
      <<   ~BODY
      << ~HTML;
  }
}
