// file      : brep/package-version-details.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/package-version-details>

#include <string>
#include <memory>    // make_shared()
#include <cassert>
#include <stdexcept> // invalid_argument

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
  void package_version_details::
  init (scanner& s)
  {
    MODULE_DIAG;

    options_ = make_shared<options::package_version_details> (
      s, unknown_mode::fail, unknown_mode::fail);

    db_ = shared_database (options_->db_host (), options_->db_port ());
  }

  void package_version_details::
  handle (request& rq, response& rs)
  {
    using namespace xml;
    using namespace web;
    using namespace web::xhtml;

    MODULE_DIAG;

    path::reverse_iterator i (rq.path ().rbegin ());
    version ver;

    try
    {
      ver = version (*i++);
    }
    catch (const invalid_argument& )
    {
      throw invalid_request (400, "invalid package version format");
    }

    assert (i != rq.path ().rend ());
    const string& package (*i);

    params::package_version_details pr;

    try
    {
      param_scanner s (rq.parameters ());
      pr = params::package_version_details (
        s, unknown_mode::fail, unknown_mode::fail);
    }
    catch (const unknown_argument& e)
    {
      throw invalid_request (400, e.what ());
    }

    const char* ident ("\n      ");
    const string name (package + "-" + ver.string ());
    const string title ("Package Version " + name);
    serializer s (rs.content (), title);

    s << HTML
      <<   HEAD
      <<     TITLE << title << ~TITLE
      <<     CSS_STYLE << ident
      <<       "a {text-decoration: none;}" << ident
      <<       "a:hover {text-decoration: underline;}" << ident
      <<       ".name {font-size: xx-large; font-weight: bold;}"
      <<     ~CSS_STYLE
      <<   ~HEAD
      <<   BODY;

    s << DIV(CLASS="name")
      <<   name
      << ~DIV;

    s <<   ~BODY
      << ~HTML;
  }
}
