// file      : brep/package-version-search.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/package-version-search>

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

    const char* title ("Package");
    serializer s (rs.content (), title);

    s << HTML
      <<   HEAD
      <<     TITLE << title << ~TITLE
      <<   ~HEAD
      <<   BODY << name << ~BODY
      << ~HTML;
  }
}
