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

  template <typename T>
  static inline query<T>
  search_params (const string& n, const string& q)
  {
    using query = query<T>;

    return "(" +
      (q.empty ()
       ? query ("NULL")
       : "plainto_tsquery (" + query::_val (q) + ")") +
      "," +
      query::_val (n) +
      ")";
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
      <<       ".url, .email {font-size: medium;}" << ident
      <<       ".comment {font-size: small;}" << ident
      <<       "#description {margin: 0.5em 0 0;}" << ident
      <<       ".tags {margin: 0.3em 0 0;}" << ident
      <<       "#versions {font-size: x-large; margin: 0.5em 0 0;}" << ident
      <<       ".package_version {margin: 0.5em 0 0;}" << ident
      <<       ".version {font-size: x-large;}" << ident
      <<       ".priority {margin: 0.3em 0 0;}" << ident
      <<       "form {margin:  0.5em 0 0 0;}"
      <<     ~CSS_STYLE
      <<   ~HEAD
      <<   BODY
      <<     DIV(ID="name") << name << ~DIV;

    const string& sq (pr.query ()); // Search query.
    size_t rop (options_->results_on_page ());

    transaction t (db_->begin ());

    shared_ptr<package> p;
    {
      latest_package lp;
      if (!db_->query_one<latest_package> (
            query<latest_package>(
              "(" + query<latest_package>::_val (name) + ")"), lp))
      {
        throw invalid_request (404, "Package '" + name + "' not found");
      }

      p = db_->load<package> (lp.id);
    }

    s << DIV(ID="summary") << p->summary << ~DIV
      << DIV_URL (p->url)
      << DIV_EMAIL (p->email);

    if (p->description)
      s << DIV(ID="description") << *p->description << ~DIV;

    s << DIV_TAGS (p->tags);

    size_t pvc (
      db_->query_value<package_count> (
        search_params<package_count> (name, sq)));

    s << DIV(ID="versions") << "Versions (" << pvc << ")" << ~DIV
      << FORM_SEARCH (sq);

    auto r (
      db_->query<package_search_rank> (
        search_params<package_search_rank> (name, sq) +
        "ORDER BY rank DESC, version_epoch DESC, "
        "version_canonical_upstream DESC, version_revision DESC" +
        "OFFSET" + to_string (pr.page () * rop) +
        "LIMIT" + to_string (rop)));

    for (const auto& pr: r)
    {
      shared_ptr<package> p (db_->load<package> (pr.id));
      const string& v (p->version.string ());

      s << DIV(CLASS="package_version")
        <<   DIV(CLASS="version")
        <<     A
        <<     HREF << "/go/" << mime_url_encode (name) << "/" << v << ~HREF
        <<       v
        <<     ~A
        <<   ~DIV
        <<   DIV_PRIORITY (p->priority)
        <<   DIV_LICENSES (p->license_alternatives)
        <<   DIV(CLASS="dependencies")
        <<     "Dependencies: " << p->dependencies.size ()
        <<   ~DIV
        << ~DIV;
    }

    t.commit ();

    string u (mime_url_encode (name));
    if (!sq.empty ())
      u += "?q=" + mime_url_encode (sq);

    s <<     DIV_PAGER (pr.page (), pvc, rop, options_->pages_in_pager (), u)
      <<   ~BODY
      << ~HTML;
  }
}
