// file      : brep/repository-details.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/repository-details>

#include <xml/serializer>

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

void brep::repository_details::
init (scanner& s)
{
  MODULE_DIAG;

  options_ = make_shared<options::repository_details> (
    s, unknown_mode::fail, unknown_mode::fail);

  if (options_->root ().empty ())
    options_->root (dir_path ("/"));

  db_ = shared_database (*options_);
}

bool brep::repository_details::
handle (request& rq, response& rs)
{
  using namespace web::xhtml;

  MODULE_DIAG;

  // The module options object is not changed after being created once per
  // server process.
  //
  static const dir_path& root (options_->root ());

  // Make sure no parameters passed.
  //
  try
  {
    name_value_scanner s (rq.parameters ());
    params::repository_details (s, unknown_mode::fail, unknown_mode::fail);
  }
  catch (const unknown_argument& e)
  {
    throw invalid_request (400, e.what ());
  }

  static const string title ("About");
  xml::serializer s (rs.content (), title);

  s << HTML
    <<   HEAD
    <<     TITLE << title << ~TITLE
    <<     CSS_LINKS (path ("repository-details.css"), root)
    <<   ~HEAD
    <<   BODY
    <<     DIV_HEADER (root)
    <<     DIV(ID="content");

  transaction t (db_->begin ());

  using query = query<repository>;

  for (const auto& r:
         db_->query<repository> (
           query::internal + "ORDER BY" + query::priority))
  {
    //@@ Feels like a lot of trouble (e.g., id_attribute()) for very
    //   dubious value. A link to the package search page just for
    //   this repository would probably be more useful.
    //
    string id (html_id (r.name));
    s << H1(ID=id)
      <<   A(HREF="#" + web::mime_url_encode (id)) << r.display_name << ~A
      << ~H1;

    if (r.summary)
      s << H2 << *r.summary << ~H2;

    if (r.description)
      s << P_DESCRIPTION (*r.description);

    if (r.email)
      s << P << A(HREF="mailto:" + *r.email) << *r.email << ~A << ~P;
  }

  t.commit ();

  s <<     ~DIV
    <<   ~BODY
    << ~HTML;

  return true;
}
