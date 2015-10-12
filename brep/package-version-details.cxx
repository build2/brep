// file      : brep/package-version-details.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/package-version-details>

#include <string>
#include <memory>    // shared_ptr, make_shared()
#include <cassert>
#include <stdexcept> // invalid_argument

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

    auto i (rq.path ().rbegin ());
    version v;

    try
    {
      v = version (*i++);
    }
    catch (const invalid_argument& )
    {
      throw invalid_request (400, "invalid package version format");
    }

    assert (i != rq.path ().rend ());
    const string& n (*i);

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
    const string& vs (v.string ());
    const string name (n + " " + vs);
    const string title ("Package Version " + name);
    serializer s (rs.content (), title);

    s << HTML
      <<   HEAD
      <<     TITLE << title << ~TITLE
      <<     CSS_STYLE << ident
      <<       A_STYLE () << ident
      <<       "#name {font-size: xx-large; font-weight: bold;}" << ident
      <<       ".url, .email {font-size: medium;}" << ident
      <<       ".comment {font-size: small;}" << ident
      <<       "#summary {font-size: x-large; margin: 0.2em 0 0;}" << ident
      <<       "#description {margin: 0.5em 0 0;}" << ident
      <<       ".tags {margin: 0.3em 0 0;}" << ident
      <<       "#package, .priority, #licenses, #dependencies, #requirements, "
               "#locations, #changes {" << ident
      <<       "  font-size: x-large;" << ident
      <<       "  margin: 0.5em 0 0;" << ident
      <<       "}" << ident
      <<       "ul {margin: 0; padding: 0 0 0 1em;}" << ident
      <<       "li {font-size: large; margin: 0.1em 0 0;}" << ident
      <<       ".conditional {font-weight: bold;}" << ident
      <<       "pre {font-size: medium; margin: 0.1em 0 0 1em;}"
      <<     ~CSS_STYLE
      <<   ~HEAD
      <<   BODY
      <<     DIV(ID="name")
      <<       A << HREF << "/go/" << mime_url_encode (n) << ~HREF << n << ~A
      <<       " " << vs
      <<     ~DIV;

    bool not_found (false);
    shared_ptr<package> p;

    transaction t (db_->begin ());

    try
    {
      p = db_->load<package> (package_id (n, v));

      // If the requested package turned up to be an "external" one just
      // respond that no "internal" package is present.
      //
      not_found = p->internal_repository == nullptr;
    }
    catch (const object_not_persistent& )
    {
      not_found = true;
    }

    if (not_found)
      throw invalid_request (404, "Package '" + name + "' not found");

    assert (p->location);
    const string u (p->internal_repository.load ()->location.string () +
                    "/" + p->location->string ());

    s << DIV(CLASS="url") << A << HREF << u << ~HREF << u << ~A << ~DIV
      << DIV(ID="summary") << p->summary << ~DIV
      << DIV_URL (p->url)
      << DIV_EMAIL (p->email);

    if (p->description)
      s << DIV(ID="description") << *p->description << ~DIV;

    const priority& pt (p->priority);

    s << DIV_TAGS (p->tags)
      << DIV_PRIORITY (pt);

    if (!pt.comment.empty ())
      s << DIV(CLASS="comment") << pt.comment << ~DIV;

    const auto& ls (p->license_alternatives);

    s << DIV(ID="licenses")
      <<   "Licenses:"
      <<   UL;

    for (const auto& la: ls)
    {
      s << LI;

      for (const auto& l: la)
      {
        if (&l != &la[0])
          s << " & ";

        s << l;
      }

      if (!la.comment.empty ())
        s << DIV(CLASS="comment") << la.comment << ~DIV;

      s << ~LI;
    }

    s <<   ~UL
      << ~DIV;

    const auto& ds (p->dependencies);

    if (!ds.empty ())
    {
      s << DIV(ID="dependencies")
        <<   "Dependencies:"
        <<   UL;

      for (const auto& da: ds)
      {
        s << LI;

        if (da.conditional)
          s << SPAN(CLASS="conditional") << "? " << ~SPAN;

        for (const auto& d: da)
        {
          if (&d != &da[0])
            s << " | ";

          // @@ Should it be a link to the package version search page or
          //    the best matching package version details page on the
          //    corresponding repository site ?
          //
          s << d;
        }

        if (!da.comment.empty ())
          s << DIV(CLASS="comment") << da.comment << ~DIV;

        s << ~LI;
      }

      s <<   ~UL
        << ~DIV;
    }

    const auto& rm (p->requirements);

    if (!rm.empty ())
    {
      s << DIV(ID="requirements")
        <<   "Requirements:"
        <<   UL;

      for (const auto& ra: rm)
      {
        s << LI;

        if (ra.conditional)
          s << SPAN(CLASS="conditional") << "? " << ~SPAN;

        if (ra.empty ())
          // If there is no requirement alternatives specified, then
          // print the comment instead.
          //
          s << ra.comment;
        else
        {
          for (const auto& r: ra)
          {
            if (&r != &ra[0])
              s << " | ";

            s << r;
          }

          if (!ra.comment.empty ())
            s << DIV(CLASS="comment") << ra.comment << ~DIV;
        }

        s << ~LI;
      }

      s <<   ~UL
        << ~DIV;
    }

    if (p->package_url || p->package_email)
    {
      s << DIV(ID="package")
        <<   "Package:"
        <<   UL;

      if (p->package_url)
        s << LI << DIV_URL (*p->package_url) << ~LI;

      if (p->package_email)
        s << LI << DIV_EMAIL (*p->package_email) << ~LI;

      s <<   ~UL
        << ~DIV;
    }

    const auto& er (p->external_repositories);

    if (!er.empty ())
    {
      s << DIV(ID="locations")
        <<   "Alternative Locations:"
        <<   UL;

      for (const auto& r: er)
      {
        repository_location  l (move (r.load ()->location));
        assert (l.remote ());

        string u ("http://" + l.host ());
        if (l.port () != 0)
          u += ":" + to_string (l.port ());

        u += "/go/" + mime_url_encode (n) + "/" + vs;
        s << LI
          <<   DIV(CLASS="url") << A << HREF << u << ~HREF << u << ~A << ~DIV
          << ~LI;
      }

      s <<   ~UL
        << ~DIV;
    }

    t.commit ();

    const string& ch (p->changes);

    if (!ch.empty ())
      s << DIV(ID="changes") << "Changes:" << PRE << ch << ~PRE << ~DIV;

    s <<   ~BODY
      << ~HTML;
  }
}
