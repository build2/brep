// file      : mod/mod-package-version-details.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-package-version-details>

#include <xml/serializer>

#include <odb/session.hxx>
#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <web/xhtml>
#include <web/module>
#include <web/xhtml-fragment>
#include <web/mime-url-encoding>

#include <brep/package>
#include <brep/package-odb>

#include <mod/page>
#include <mod/options>

using namespace std;
using namespace odb::core;
using namespace brep::cli;

// While currently the user-defined copy constructor is not required (we don't
// need to deep copy nullptr's), it is a good idea to keep the placeholder
// ready for less trivial cases.
//
brep::package_version_details::
package_version_details (const package_version_details& r)
    : database_module (r),
      options_ (r.initialized_ ? r.options_ : nullptr)
{
}

void brep::package_version_details::
init (scanner& s)
{
  MODULE_DIAG;

  options_ = make_shared<options::package_version_details> (
    s, unknown_mode::fail, unknown_mode::fail);

  database_module::init (*options_);

  if (options_->root ().empty ())
    options_->root (dir_path ("/"));
}

bool brep::package_version_details::
handle (request& rq, response& rs)
{
  using namespace web;
  using namespace web::xhtml;
  using brep::version; // Not to confuse with module::version.

  MODULE_DIAG;

  const dir_path& root (options_->root ());

  auto i (rq.path ().rbegin ());
  version ver;

  try
  {
    ver = version (*i++);
  }
  catch (const invalid_argument& )
  {
    throw invalid_request (400, "invalid package version format");
  }

  const string& sver (ver.string ());

  assert (i != rq.path ().rend ());
  const string& name (*i);

  params::package_version_details params;
  bool full;

  try
  {
    name_value_scanner s (rq.parameters ());
    params = params::package_version_details (
      s, unknown_mode::fail, unknown_mode::fail);

    full = params.form () == page_form::full;
  }
  catch (const cli::exception& e)
  {
    throw invalid_request (400, e.what ());
  }

  auto url = [&sver] (bool f = false, const string& a = "") -> string
  {
    string u (sver);

    if (f)           { u += "?f=full"; }
    if (!a.empty ()) { u += '#' + a; }
    return u;
  };

  const string title (name + " " + sver);
  xml::serializer s (rs.content (), title);

  s << HTML
    <<   HEAD
    <<     TITLE << title << ~TITLE
    <<     CSS_LINKS (path ("package-version-details.css"), root)
    <<   ~HEAD
    <<   BODY
    <<     DIV_HEADER (root, options_->logo (), options_->menu ())
    <<     DIV(ID="content");

  if (full)
    s << CLASS("full");

  s <<       DIV(ID="heading")
    <<         H1
    <<           A(HREF=root / path (mime_url_encode (name))) << name << ~A
    <<           "/"
    <<           A(HREF=url ()) << sver << ~A
    <<         ~H1
    <<         A(HREF=url (!full)) << (full ? "[brief]" : "[full]") << ~A
    <<       ~DIV;

  bool not_found (false);
  shared_ptr<package> pkg;

  session sn;
  transaction t (db_->begin ());

  try
  {
    pkg = db_->load<package> (package_id (name, ver));

    // If the requested package turned up to be an "external" one just
    // respond that no "internal" package is present.
    //
    not_found = !pkg->internal ();
  }
  catch (const object_not_persistent& )
  {
    not_found = true;
  }

  if (not_found)
    throw invalid_request (404, "Package '" + title + "' not found");

  s << H2 << pkg->summary << ~H2;

  static const string id ("description");
  if (const auto& d = pkg->description)
    s << (full
          ? P_DESCRIPTION (*d, id)
          : P_DESCRIPTION (*d, options_->package_description (),
                           url (!full, id)));

  assert (pkg->location && pkg->sha256sum);

  const repository_location& rl (pkg->internal_repository.load ()->location);

  s << TABLE(CLASS="proplist", ID="version")
    <<   TBODY

    // Repeat version here since it can be cut out in the header.
    //
    <<     TR_VERSION (pkg->version)

    <<     TR_PRIORITY (pkg->priority)
    <<     TR_LICENSES (pkg->license_alternatives)
    <<     TR_REPOSITORY (rl.canonical_name (), root)
    <<     TR_LOCATION (rl)
    <<     TR_DOWNLOAD (rl.string () + "/" + pkg->location->string ())
    <<     TR_SHA256SUM (*pkg->sha256sum)
    <<   ~TBODY
    << ~TABLE

    << TABLE(CLASS="proplist", ID="package")
    <<   TBODY
    <<     TR_URL (pkg->url)
    <<     TR_EMAIL (pkg->email);

  const auto& pu (pkg->package_url);
  if (pu && *pu != pkg->url)
    s << TR_URL (*pu, "pkg-url");

  const auto& pe (pkg->package_email);
  if (pe && *pe != pkg->email)
    s << TR_EMAIL (*pe, "pkg-email");

  s <<     TR_TAGS (pkg->tags, root)
    <<   ~TBODY
    << ~TABLE;

  const auto& ds (pkg->dependencies);
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

      if (da.buildtime)
        s << "*";

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

        const auto& dcon (d.constraint);
        const string& dname (p->id.name);
        string ename (mime_url_encode (dname));

        if (r->url)
        {
          string u (*r->url + ename);
          s << A(HREF=u) << dname << ~A;

          if (dcon)
            s << ' ' << A(HREF=u + "/" + p->version.string ()) << *dcon << ~A;
        }
        else if (p->internal ())
        {
          dir_path u (root / dir_path (ename));
          s << A(HREF=u) << dname << ~A;

          if (dcon)
            s << ' ' << A(HREF=u / path (p->version.string ())) << *dcon << ~A;
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

  const auto& rm (pkg->requirements);
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

      if (ra.buildtime)
        s << "*";

      if (ra.conditional || ra.buildtime)
        s << " ";

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

  const auto& ch (pkg->changes);
  if (!ch.empty ())
    s << H3 << "Changes" << ~H3
      << (full
          ? PRE_CHANGES (ch)
          : PRE_CHANGES (ch,
                         options_->package_changes (),
                         url (!full, "changes")));

  s <<     ~DIV
    <<   ~BODY
    << ~HTML;

  return true;
}
