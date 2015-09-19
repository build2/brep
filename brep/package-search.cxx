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
      <<       A_STYLE () << ident
      <<       PAGER_STYLE () << ident
      <<       ".packages {font-size: x-large;}" << ident
      <<       ".package {margin: 0.5em 0 0;}" << ident
      <<       ".name {font-size: x-large;}" << ident
      <<       ".dependencies {margin: 0.3em 0 0;}" << ident
      <<     ~CSS_STYLE
      <<   ~HEAD
      <<   BODY;

    string q (
      pr.query ().empty () ? "" : "q=" + mime_url_encode (pr.query ()));

    size_t rop (options_->results_on_page ());

    transaction t (db_->begin ());

    // @@ Query will include search criteria if specified.
    //
    size_t pc (db_->query_value<internal_package_count> ().count);

    s << DIV(CLASS="packages")
      <<   "Packages (" << pc << ")"
      << ~DIV;

    // @@ Use appropriate view when clarify which package info to be displayed
    //    and search index structure get implemented. Query will also
    //    include search criteria if specified.
    //
    using query = query<internal_package>;

    auto r (
      db_->query<internal_package> (
        "ORDER BY" + query::package::name +
        "OFFSET" + to_string (pr.page () * rop) +
        "LIMIT" + to_string (rop)));

    for (const auto& ip: r)
    {
      const package& p = *ip.package;
      const package_version& v = *ip.version;

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
        <<   DIV(CLASS="dependencies")
        <<     "Dependencies: " << v.dependencies.size ()
        <<   ~DIV
        <<   LICENSES (v.license_alternatives)
        <<   TAGS (p.tags);

      s << ~DIV;
    }

    t.commit ();

    auto u (
      [&q](size_t p)
      {
        string url ("/");
        if (p > 0)
          url += "?p=" + to_string (p);

        if (!q.empty ())
          url += string (p > 0 ? "&" : "?") + q;

        return url;
      });

    s <<      PAGER (pr.page (), pc, rop, options_->pages_in_pager (), u)
      <<   ~BODY
      << ~HTML;
  }
}
