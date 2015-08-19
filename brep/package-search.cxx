// file      : brep/package-search.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/package-search>

#include <string>
#include <memory> // make_shared()

#include <xml/serializer>

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <web/xhtml>
#include <web/module>
#include <web/mime-url-encoding>

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
      <<     ".package {margin: 0 0 0.5em;}" << ident
      <<     ".name a {text-decoration: none;}" << ident
      <<     ".summary {font-size: small;}"
      <<     ~CSS_STYLE
      <<   ~HEAD
      <<   BODY;

    string q (
      pr.query ().empty () ? "" : "q=" + mime_url_encode (pr.query ()));

    transaction t (db_->begin ());

    // @@ Use appropriate view when clarify which package info to be displayed
    //    and search index structure get implemented.
    //
    using query = query<package>;
    auto r (
      db_->query<package> (
        "ORDER BY" + query::name +
        "OFFSET" + to_string (pr.page () * options_->results_on_page ()) +
        "LIMIT" + to_string (options_->results_on_page ())));

    for (const auto& p: r)
    {
      s << DIV(CLASS="package")
        <<   DIV(CLASS="name")
        <<     A
        <<     HREF << "/go/" << mime_url_encode (p.name);

      // Propagate search criteria to the package version search url.
      //
      if (!q.empty ())
        s << "?" << q;

      s <<     ~HREF
        <<       p.name
        <<     ~A
        <<   ~DIV
        <<   DIV(CLASS="summary")
        <<     p.summary
        <<   ~DIV
        << ~DIV;
    }

    t.commit ();

    if (pr.page () || r.size () == options_->results_on_page ())
    {
      s << DIV;

      if (pr.page ())
        s << A
          << HREF << "/?p=" << pr.page () - 1 << (q.empty () ? "" : "&" + q)
          << ~HREF
          <<   "Previous"
          << ~A
          << " ";

      // @@ Not ideal as can produce link to an empty page, but easy to fix
      //    and most likelly will be replaced with something more meaningful
      //    based on knowing the total number of matched packages.
      //
      if (r.size () == options_->results_on_page ())
        s << A
          << HREF << "/?p=" << pr.page () + 1 << (q.empty () ? "" : "&" + q)
          << ~HREF
          <<   "Next"
          << ~A;

      s << ~DIV;
    }

    s <<   ~BODY
      << ~HTML;
  }
}
