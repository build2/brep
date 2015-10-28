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

    bool f (pr.full ());
    const string& vs (v.string ());

    auto url ([&vs](bool f = false, const string& a = "") -> string
      {
        string u (vs);

        if (f)           { u += "?full"; }
        if (!a.empty ()) { u += '#' + a; }
        return u;
      });

    const string name (n + " " + vs);
    serializer s (rs.content (), name);

    s << HTML
      <<   HEAD
      <<     TITLE << name << ~TITLE
      <<     CSS_LINKS ("/package-version-details.css")
      <<   ~HEAD
      <<   BODY
      <<     DIV_HEADER ()
      <<     DIV(ID="content");

    if (f)
      s << CLASS << "full" << ~CLASS;

    s <<       DIV(ID="heading")
      <<         H1
      <<           A
      <<           HREF << "/go/" << mime_url_encode (n) << ~HREF
      <<             n
      <<           ~A
      <<           "/"
      <<           A(HREF=url ()) << vs << ~A
      <<         ~H1
      <<         A(HREF=url (!f)) << (f ? "[brief]" : "[full]") << ~A
      <<       ~DIV;

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

    s << H2 << p->summary << ~H2;

    if (const auto& d = p->description)
      s << (f
            ? P_DESCRIPTION (*d)
            : P_DESCRIPTION (
                *d, options_->description_length (), url (!f, "description")));

    // Link to download from the internal repository.
    //
    assert (p->location);
    const string du (p->internal_repository.load ()->location.string () +
                     "/" + p->location->string ());

    t.commit ();

    s << TABLE(CLASS="proplist", ID="version")
      <<   TBODY

      // Repeat version here since it can be cut out in the header.
      //
      <<     TR_VERSION (p->version.string ())

      <<     TR_PRIORITY (p->priority)
      <<     TR_LICENSES (p->license_alternatives)
      <<     TR_LOCATION (p->internal_repository.object_id ())
      <<     TR_DOWNLOAD (du)
      <<   ~TBODY
      << ~TABLE

      << TABLE(CLASS="proplist", ID="package")
      <<   TBODY
      <<     TR_URL (p->url)
      <<     TR_EMAIL (p->email);

    if (p->package_url && *p->package_url != p->url)
      s << TR_URL (*p->package_url, "pkg-url");

    if (p->package_email && *p->package_email != p->email)
      s << TR_EMAIL (*p->package_email, "pkg-email");

    s <<     TR_TAGS (p->tags)
      <<   ~TBODY
      << ~TABLE;

    const auto& ds (p->dependencies);

    if (!ds.empty ())
    {
      s << H3 << "Depends" << ~H3
        << TABLE(CLASS="proplist", ID="depends")
        <<   TBODY;

      for (const auto& da: ds)
      {
        s << TR(CLASS="depends")
          <<   TH;

        if (da.conditional)
          s << "?";

        s <<   ~TH
          <<   TD
          <<     SPAN(CLASS="value");

        for (const auto& d: da)
        {
          if (&d != &da[0])
            s << " | ";

          s << d; // @@ Should it be a link ?
        }

        s <<     ~SPAN
          <<     SPAN_COMMENT (da.comment)
          <<   ~TD
          << ~TR;
      }

      s <<   ~TBODY
        << ~TABLE;
    }

    const auto& rt (p->requirements);

    if (!rt.empty ())
    {
      s << H3 << "Requires" << ~H3
        << TABLE(CLASS="proplist", ID="requires")
        <<   TBODY;

      for (const auto& ra: rt)
      {
        s << TR(CLASS="requires")
          <<   TH;

        if (ra.conditional)
          s << "?";

        s <<   ~TH
          <<   TD
          <<     SPAN(CLASS="value");

        for (const auto& r: ra)
        {
          if (&r != &ra[0])
            s << " | ";

          s << r;
        }

        s <<     ~SPAN
          <<     SPAN_COMMENT (ra.comment)
          <<   ~TD
          << ~TR;
      }

      s <<   ~TBODY
        << ~TABLE;
    }

    const string& ch (p->changes);

    if (!ch.empty ())
      s << H3 << "Changes" << ~H3
        << (f
            ? PRE_CHANGES (ch)
            : PRE_CHANGES (
                ch, options_->changes_length (), url (!f, "changes")));

    s <<     ~DIV
      <<   ~BODY
      << ~HTML;
  }
}
