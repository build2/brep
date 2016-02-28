// file      : brep/mod-package-search.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/mod-package-search>

#include <xml/serializer>

#include <odb/session.hxx>
#include <odb/database.hxx>
#include <odb/transaction.hxx>
#include <odb/schema-catalog.hxx>

#include <web/xhtml>
#include <web/module>
#include <web/xhtml-fragment>
#include <web/mime-url-encoding>

#include <brep/version>

#include <brep/page>
#include <brep/options>
#include <brep/package>
#include <brep/database>
#include <brep/package-odb>

using namespace odb::core;
using namespace brep::cli;

void brep::package_search::
init (scanner& s)
{
  MODULE_DIAG;

  options_ = make_shared<options::package_search> (
    s, unknown_mode::fail, unknown_mode::fail);

  if (options_->root ().empty ())
    options_->root (dir_path ("/"));

  db_ = shared_database (*options_);

  // Check that the database schema matches the current one. It's enough to
  // perform the check in just a single module implementation (and we don't
  // do in the dispatcher because it doesn't use the database).
  //
  // Note that the failure can be reported by each web server worker process.
  // While it could be tempting to move the check to the
  // repository_root::version() function, it would be wrong. The function can
  // be called by a different process (usually the web server root one) not
  // having the proper permissions to access the database.
  //
  if (schema_catalog::current_version (*db_) != db_->schema_version ())
    fail << "database schema differs from the current one (module "
         << BREP_VERSION_STR << ")";
}

template <typename T>
static inline query<T>
search_param (const brep::string& q)
{
  using query = query<T>;
  return "(" +
    (q.empty ()
     ? query ("NULL")
     : "plainto_tsquery (" + query::_val (q) + ")") +
    ")";
}

bool brep::package_search::
handle (request& rq, response& rs)
{
  using namespace web::xhtml;

  MODULE_DIAG;

  // The module options object is not changed after being created once per
  // server process.
  //
  static const size_t res_page (options_->search_results ());
  static const dir_path& root (options_->root ());
  static const fragment& logo (options_->logo ());
  static const vector<page_menu>& menu (options_->menu ());

  params::package_search params;

  try
  {
    name_value_scanner s (rq.parameters ());
    params = params::package_search (s, unknown_mode::fail, unknown_mode::fail);
  }
  catch (const unknown_argument& e)
  {
    throw invalid_request (400, e.what ());
  }

  size_t page (params.page ());
  const string& squery (params.query ());
  string squery_param (squery.empty ()
                       ? ""
                       : "?q=" + web::mime_url_encode (squery));

  static const string title ("Packages");
  xml::serializer s (rs.content (), title);

  s << HTML
    <<   HEAD
    <<     TITLE
    <<       title;

  if (!squery.empty ())
    s << " " << squery;

  s <<     ~TITLE
    <<     CSS_LINKS (path ("package-search.css"), root)
    //
    // This hack is required to avoid the "flash of unstyled content", which
    // happens due to the presence of the autofocus attribute in the input
    // element of the search form. The problem appears in Firefox and has a
    // (4-year old, at the time of this writing) bug report:
    //
    // https://bugzilla.mozilla.org/show_bug.cgi?id=712130.
    //
    <<     SCRIPT << " " << ~SCRIPT
    <<   ~HEAD
    <<   BODY
    <<     DIV_HEADER (root, logo, menu)
    <<     DIV(ID="content");

  session sn;
  transaction t (db_->begin ());

  auto pkg_count (
    db_->query_value<latest_package_count> (
      search_param<latest_package_count> (squery)));

  s << FORM_SEARCH (squery)
    << DIV_COUNTER (pkg_count, "Package", "Packages");

  // Enclose the subsequent tables to be able to use nth-child CSS selector.
  //
  s << DIV;
    for (const auto& pr:
           db_->query<latest_package_search_rank> (
             search_param<latest_package_search_rank> (squery) +
             "ORDER BY rank DESC, name" +
             "OFFSET" + to_string (page * res_page) +
             "LIMIT" + to_string (res_page)))
  {
    shared_ptr<package> p (db_->load<package> (pr.id));

    s << TABLE(CLASS="proplist package")
      <<   TBODY
      <<     TR_NAME (p->id.name, squery_param, root)
      <<     TR_SUMMARY (p->summary)
      <<     TR_LICENSE (p->license_alternatives)
      <<     TR_TAGS (p->tags, root)
      <<     TR_DEPENDS (p->dependencies, root)
      <<     TR_REQUIRES (p->requirements)
      <<   ~TBODY
      << ~TABLE;
  }
  s << ~DIV;

  t.commit ();

  s <<       DIV_PAGER (page, pkg_count, res_page, options_->search_pages (),
                        root.string () + squery_param)
    <<     ~DIV
    <<   ~BODY
    << ~HTML;

  return true;
}
