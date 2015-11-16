// file      : brep/package-version-details.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/package-version-details>

#include <string>
#include <memory>    // shared_ptr, make_shared()
#include <cassert>
#include <stdexcept> // invalid_argument

#include <xml/serializer>

#include <odb/session.hxx>
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
    using namespace web;
    using namespace web::xhtml;

    MODULE_DIAG;

    // The module options object is not changed after being created once per
    // server process.
    //
    static const dir_path& rt (
      options_->root ().empty ()
      ? dir_path ("/")
      : options_->root ());

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

    const string& vs (v.string ());

    assert (i != rq.path ().rend ());
    const string& n (*i); // Package name.
    const string name (n + " " + vs);

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

    auto url ([&vs](bool f = false, const string& a = "") -> string
      {
        string u (vs);

        if (f)           { u += "?full"; }
        if (!a.empty ()) { u += '#' + a; }
        return u;
      });

    xml::serializer s (rs.content (), name);
    static const path go ("go");
    static const path sp ("package-version-details.css");

    s << HTML
      <<   HEAD
      <<     TITLE << name << ~TITLE
      <<     CSS_LINKS (sp, rt)
      <<   ~HEAD
      <<   BODY
      <<     DIV_HEADER (rt)
      <<     DIV(ID="content");

    if (f)
      s << CLASS("full");

    s <<       DIV(ID="heading")
      <<         H1
      <<           A(HREF=rt / go / path (mime_url_encode (n))) << n << ~A
      <<           "/"
      <<           A(HREF=url ()) << vs << ~A
      <<         ~H1
      <<         A(HREF=url (!f)) << (f ? "[brief]" : "[full]") << ~A
      <<       ~DIV;

    bool not_found (false);
    shared_ptr<package> p;

    session sn;
    transaction t (db_->begin ());

    try
    {
      p = db_->load<package> (package_id (n, v));

      // If the requested package turned up to be an "external" one just
      // respond that no "internal" package is present.
      //
      not_found = !p->internal ();
    }
    catch (const object_not_persistent& )
    {
      not_found = true;
    }

    if (not_found)
      throw invalid_request (404, "Package '" + name + "' not found");

    s << H2 << p->summary << ~H2;

    static const size_t dl (options_->description_length ());

    if (const auto& d = p->description)
      s << (f
            ? P_DESCRIPTION (*d)
            : P_DESCRIPTION (*d, dl, url (!f, "description")));

    // Link to download from the internal repository.
    //
    assert (p->location);
    const string du (p->internal_repository.load ()->location.string () + "/" +
                     p->location->string ());

    s << TABLE(CLASS="proplist", ID="version")
      <<   TBODY

      // Repeat version here since it can be cut out in the header.
      //
      <<     TR_VERSION (p->version.string ())

      <<     TR_PRIORITY (p->priority)
      <<     TR_LICENSES (p->license_alternatives)
      <<     TR_LOCATION (p->internal_repository.object_id (), rt)
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

    s <<     TR_TAGS (p->tags, rt)
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

          shared_ptr<package> p (d.package.load ());
          assert (p->internal () || !p->other_repositories.empty ());

          shared_ptr<repository> r (
            p->internal ()
            ? p->internal_repository.load ()
            : p->other_repositories[0].load ());

          const auto& dc (d.constraint);
          const string& dn (p->id.name);
          string en (mime_url_encode (dn));

          if (r->url)
          {
            string u (*r->url + "go/" + en);
            s << A(HREF=u) << dn << ~A;

            if (dc)
              s << ' '
                << A
                << HREF << u << "/" << p->version.string () << ~HREF
                <<   *dc
                << ~A;
          }
          else if (p->internal ())
          {
            path u (rt / go / path (en));
            s << A(HREF=u) << dn << ~A;

            if (dc)
              s << ' ' << A(HREF=u / path (p->version.string ())) << *dc << ~A;
          }
          else
            // Display the dependency as a plain text if no repository URL
            // available.
            //
            s << d;
        }

        s <<     ~SPAN
          <<     SPAN_COMMENT (da.comment)
          <<   ~TD
          << ~TR;
      }

      s <<   ~TBODY
        << ~TABLE;
    }

    t.commit ();

    const auto& rm (p->requirements);

    if (!rm.empty ())
    {
      s << H3 << "Requires" << ~H3
        << TABLE(CLASS="proplist", ID="requires")
        <<   TBODY;

      for (const auto& ra: rm)
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

    static const size_t cl (options_->changes_length ());
    const auto& ch (p->changes);

    if (!ch.empty ())
      s << H3 << "Changes" << ~H3
        << (f
            ? PRE_CHANGES (ch)
            : PRE_CHANGES (ch, cl, url (!f, "changes")));

    s <<     ~DIV
      <<   ~BODY
      << ~HTML;
  }
}
