// file      : brep/package-search.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/package-search>

#include <xml/serializer>

#include <odb/session.hxx>
#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <web/xhtml>
#include <web/module>
#include <web/mime-url-encoding>

#include <brep/types>
#include <brep/utility>

#include <brep/page>
#include <brep/options>
#include <brep/package>
#include <brep/package-odb>
#include <brep/shared-database>

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
    <<   ~HEAD
    <<   BODY
    <<     DIV_HEADER (root)
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
