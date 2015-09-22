// file      : brep/package-version-search.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/package-version-search>

#include <string>
#include <memory>  // make_shared(), shared_ptr
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
      <<       A_STYLE () << ident
      <<       DIV_PAGER_STYLE () << ident
      <<       "#name {font-size: xx-large; font-weight: bold;}" << ident
      <<       "#summary {font-size: x-large; margin: 0.2em 0 0;}" << ident
      <<       ".url {margin: 0.3em 0 0;}" << ident
      <<       "#description {margin: 0.5em 0 0;}" << ident
      <<       ".tags {margin: 0.3em 0 0;}" << ident
      <<       "#versions {font-size: x-large; margin: 0.5em 0 0;}" << ident
      <<       ".package_version {margin: 0.5em 0 0;}" << ident
      <<       ".version {font-size: x-large;}" << ident
      <<       ".priority {margin: 0.3em 0 0;}"
      <<     ~CSS_STYLE
      <<   ~HEAD
      <<   BODY
      <<     DIV(ID="name") << name << ~DIV;

    shared_ptr<package> p;
    size_t rop (options_->results_on_page ());

    transaction t (db_->begin ());

    try
    {
      p = db_->load<package> (name);
    }
    catch (const object_not_persistent& )
    {
      throw invalid_request (404, "Package '" + name + "' not found");
    }

    s << DIV(ID="summary") << p->summary << ~DIV
      << DIV(CLASS="url")
      <<   A << HREF << p->url << ~HREF << p->url << ~A
      << ~DIV
      << DIV(CLASS="email")
      <<   A << HREF << "mailto:" << p->email << ~HREF << p->email << ~A
      << ~DIV;

    if (p->description)
      s << DIV(ID="description") << *p->description << ~DIV;

    s << DIV_TAGS (p->tags);

    size_t pvc;
    {
      using query = query<package_version_count>;

      // @@ Query will also include search criteria if specified.
      //
      pvc = db_->query_value<package_version_count> (
        query::id.data.package == name &&
        query::internal_repository.is_not_null ()).count;
    }

    s << DIV(ID="versions") << "Versions (" << pvc << ")" << ~DIV;

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

    // @@ Use appropriate view when clarify which package version info to be
    //    displayed and search index structure get implemented. Query will also
    //    include search criteria if specified.
    //
    using query = query<package_version>;
    auto r (
      db_->query<package_version> (
        (query::id.data.package == name &&
         query::internal_repository.is_not_null ()) +
        "ORDER BY" + query::id.data.epoch + "DESC," +
        query::id.data.canonical_upstream + "DESC," +
        query::id.data.revision + "DESC " +
        "OFFSET" + to_string (pr.page () * rop) +
        "LIMIT" + to_string (rop)));

    for (const auto& v: r)
    {
      const string& vs (v.version.string ());

      s << DIV(CLASS="package_version")
        <<   DIV(CLASS="version")
        <<     A
        <<     HREF << "/go/" << mime_url_encode (name) << "/" << vs << ~HREF
        <<       vs
        <<     ~A
        <<   ~DIV
        <<   DIV_PRIORITY (v.priority)
        <<   DIV_LICENSES (v.license_alternatives)
        <<   DIV(CLASS="dependencies")
        <<     "Dependencies: " << v.dependencies.size ()
        <<   ~DIV
        << ~DIV;
    }

    t.commit ();

    string u (mime_url_encode (name));
    if (!pr.query ().empty ())
      u += "?q=" + mime_url_encode (pr.query ());

    s <<     DIV_PAGER (pr.page (), pvc, rop, options_->pages_in_pager (), u)
      <<   ~BODY
      << ~HTML;
  }
}
