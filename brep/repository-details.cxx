// file      : brep/repository-details.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/repository-details>

#include <string>
#include <memory>  // make_shared()

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
using namespace odb::core;

namespace brep
{
  using namespace cli;

  void repository_details::
  init (scanner& s)
  {
    MODULE_DIAG;

    options_ = make_shared<options::repository_details> (
      s, unknown_mode::fail, unknown_mode::fail);

    db_ = shared_database (options_->db_host (), options_->db_port ());
  }

  void repository_details::
  handle (request&, response& rs)
  {
    using namespace web::xhtml;

    MODULE_DIAG;

    // The module options object is not changed after being created once per
    // server process.
    //
    static const dir_path& rt (
      options_->root ().empty ()
      ? dir_path ("/")
      : options_->root ());

    xml::serializer s (rs.content (), "About");
    const string& title (s.output_name ());
    static const path sp ("repository-details.css");

    s << HTML
      <<   HEAD
      <<     TITLE << title << ~TITLE
      <<     CSS_LINKS (sp, rt)
      <<   ~HEAD
      <<   BODY
      <<     DIV_HEADER (rt)
      <<     DIV(ID="content");

    transaction t (db_->begin ());

    using query = query<repository>;
    auto rp (db_->query<repository> (query::internal + "ORDER BY name"));

    for (const auto& r: rp)
    {
      string id (id_attribute (r.name));
      s << H1(ID=id)
        <<   A(HREF="#" + web::mime_url_encode (id)) << r.name << ~A
        << ~H1;

      if (r.summary)
        s << H2 << *r.summary << ~H2;

      if (r.description)
        s << P_DESCRIPTION (*r.description, false);

      if (r.email)
        s << P
          <<   A << HREF << "mailto:" << *r.email << ~HREF << *r.email << ~A
          << ~P;
    }

    t.commit ();

    s <<     ~DIV
      <<   ~BODY
      << ~HTML;
  }
}
