// file      : brep/package-version-search.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/package-version-search>

#include <string>
#include <memory>  // make_shared(), shared_ptr
#include <cstddef> // size_t
#include <cassert>

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
  void package_version_search::
  init (scanner& s)
  {
    MODULE_DIAG;

    options_ = make_shared<options::package_version_search> (
      s, unknown_mode::fail, unknown_mode::fail);

    db_ = shared_database (options_->db_host (), options_->db_port ());
  }

  void package_version_search::
  handle (request& rq, response& rs)
  {
    using namespace xml;
    using namespace web;
    using namespace web::xhtml;

    MODULE_DIAG;

    const string& name (*rq.path ().rbegin ());
    params::package_version_search pr;

    try
    {
      param_scanner s (rq.parameters ());
      pr = params::package_version_search (
        s, unknown_mode::fail, unknown_mode::fail);
    }
    catch (const unknown_argument& e)
    {
      throw invalid_request (400, e.what ());
    }

    const char* ident ("\n      ");
    const string title ("Package " + name);
    serializer s (rs.content (), title);

    s << HTML
      <<   HEAD
      <<     TITLE << title << ~TITLE
      <<     CSS_STYLE << ident
      <<       pager_style () << ident
      <<       "a {text-decoration: none;}" << ident
      <<       "a:hover {text-decoration: underline;}" << ident
      <<       ".name {font-size: xx-large; font-weight: bold;}" << ident
      <<       ".summary {font-size: x-large; margin: 0.2em 0 0;}" << ident
      <<       ".url {font-size: small;}" << ident
      <<       ".email {font-size: small;}" << ident
      <<       ".description {margin: 0.5em 0 0;}" << ident
      <<       ".tags {margin: 0.5em 0 0;}" << ident
      <<       ".tag {padding: 0 0.3em 0 0;}" << ident
      <<       ".versions {font-size: x-large; margin: 0.5em 0 0;}" << ident
      <<       ".package_version {margin: 0.5em 0 0;}" << ident
      <<       ".version {font-size: x-large;}"
      <<     ~CSS_STYLE
      <<   ~HEAD
      <<   BODY;

    transaction t (db_->begin ());

    shared_ptr<package> p;

    try
    {
      p = db_->load<package> (name);
    }
    catch (const object_not_persistent& )
    {
      throw invalid_request (404, "Package '" + name + "' not found");
    }

    s << DIV(CLASS="name")
      <<   name
      << ~DIV
      << DIV(CLASS="summary")
      <<   p->summary
      << ~DIV;

    s << DIV(CLASS="url")
      <<   A << HREF << p->url << ~HREF << p->url << ~A
      << ~DIV
      << DIV(CLASS="email")
      <<   A << HREF << "mailto:" << p->email << ~HREF << p->email << ~A
      << ~DIV;

    if (p->description)
      s << DIV(CLASS="description")
        <<   *p->description
        << ~DIV;

    if (!p->tags.empty ())
    {
      s << DIV(CLASS="tags");

      for (const auto& t: p->tags)
        s << SPAN(CLASS="tag") << t << ~SPAN << " ";

      s << ~DIV;
    }

    // @@ Query will also include search criteria if specified.
    //
    size_t pvc (
      db_->query_value<package_version_count> (
        query<package_version_count>::id.data.package == name).count);

    s << DIV(CLASS="versions")
      <<   "Versions (" << pvc << ")"
      << ~DIV;

    if (p->package_url)
      s << DIV(CLASS="url")
        <<   A << HREF << *p->package_url << ~HREF << *p->package_url << ~A
        << ~DIV;

    if (p->package_email)
      s << DIV(CLASS="email")
        <<   A
        <<   HREF << "mailto:" << *p->package_email << ~HREF
        <<     *p->package_email
        <<   ~A
        << ~DIV;

    size_t rop (options_->results_on_page ());

    // @@ Use appropriate view when clarify which package version info to be
    //    displayed and search index structure get implemented. Query will also
    //    include search criteria if specified.
    //
    using query = query<package_version>;
    auto r (
      db_->query<package_version> ((query::id.data.package == name) +
        "ORDER BY" + query::id.data.epoch + "DESC," +
        query::id.data.canonical_upstream + "DESC," +
        query::id.data.revision + "DESC " +
        "OFFSET" + to_string (pr.page () * rop) +
        "LIMIT" + to_string (rop)));

    for (const auto& v: r)
    {
      static const strings priority_names (
        {"low", "medium", "high", "security"});

      assert (v.priority < priority_names.size ());

      const string& vs (v.version.string ());

      s << DIV(CLASS="package_version")
        <<   DIV(CLASS="version")
        <<     A
        <<     HREF
        <<       "/go/" << mime_url_encode (name) << "/" << vs
        <<     ~HREF
        <<       vs
        <<     ~A
        <<   ~DIV
        <<   DIV(CLASS="priority")
        <<     "Priority: " << priority_names[v.priority]
        <<   ~DIV
        <<   DIV(CLASS="licenses")
        <<     "Licenses: ";

      for (const auto& la: v.license_alternatives)
      {
        if (&la != &v.license_alternatives[0])
          s << " or ";

        for (const auto& l: la)
        {
          if (&l != &la[0])
            s << ", ";

          s << l;
        }
      }

      s <<   ~DIV
        << ~DIV;
    }

    t.commit ();

    auto u (
      [&name, &pr](size_t p)
      {
        string url (name);
        if (p > 0)
          url += "?p=" + to_string (p);

        if (!pr.query ().empty ())
          url +=
            string (p > 0 ? "&" : "?") + "q=" + mime_url_encode (pr.query ());

        return url;
      });

    s <<     pager (pr.page (), pvc, rop, options_->pages_in_pager (), u)
      <<   ~BODY
      << ~HTML;
  }
}
