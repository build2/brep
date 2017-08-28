// file      : mod/mod-package-version-details.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-package-version-details.hxx>

#include <libstudxml/serializer.hxx>

#include <odb/session.hxx>
#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <libbutl/utility.hxx> // alpha(), ucase(), lcase()

#include <web/xhtml.hxx>
#include <web/module.hxx>
#include <web/mime-url-encoding.hxx>

#include <libbrep/build.hxx>
#include <libbrep/build-odb.hxx>
#include <libbrep/package.hxx>
#include <libbrep/package-odb.hxx>

#include <mod/page.hxx>
#include <mod/options.hxx>

using namespace std;
using namespace butl;
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

  database_module::init (*options_, options_->package_db_retry ());

  if (options_->build_config_specified ())
    database_module::init (static_cast<options::build>    (*options_),
                           static_cast<options::build_db> (*options_),
                           options_->build_db_retry ());

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

  const string& host (options_->host ());
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
  transaction t (package_db_->begin ());

  try
  {
    pkg = package_db_->load<package> (package_id (name, ver));

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
    <<     TR_URL (pkg->url);

  if (pkg->doc_url)
    s << TR_URL (*pkg->doc_url, "doc-url");

  if (pkg->src_url)
    s << TR_URL (*pkg->src_url, "src-url");

  const auto& pu (pkg->package_url);
  if (pu && *pu != pkg->url)
    s << TR_URL (*pu, "package-url");

  const email& em (pkg->email);
  s << TR_EMAIL (em);

  const auto& pe (pkg->package_email);
  if (pe && *pe != em)
    s << TR_EMAIL (*pe, "package-email");

  const auto& be (pkg->build_email);
  if (be && ((pe && *be != *pe) || (!pe && *be != em)))
    s << TR_EMAIL (*be, "build-email");

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

  // Don't display the page builds section for stub packages.
  //
  bool builds (build_db_ != nullptr &&
               ver.compare (wildcard_version, true) != 0);

  if (builds)
    package_db_->load (*pkg, pkg->build_section);

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

  if (builds)
  {
    s << H3 << "Builds" << ~H3
      << DIV(ID="builds");

    timestamp now (system_clock::now ());
    transaction t (build_db_->begin ());

    // Print built package configurations.
    //
    using query = query<build>;

    for (auto& b: build_db_->query<build> (
           (query::id.package == pkg->id &&

            query::id.configuration.in_range (build_conf_names_->begin (),
                                              build_conf_names_->end ())) +

           "ORDER BY" + query::timestamp + "DESC"))
    {
      string ts (butl::to_string (b.timestamp,
                                  "%Y-%m-%d %H:%M:%S %Z",
                                  true,
                                  true) +
                 " (" + butl::to_string (now - b.timestamp, false) + " ago)");

      if (b.state == build_state::built)
        build_db_->load (b, b.results_section);

      s << TABLE(CLASS="proplist build")
        <<   TBODY
        <<     TR_VALUE ("toolchain",
                         b.toolchain_name + '-' +
                         b.toolchain_version.string ())
        <<     TR_VALUE ("config",
                         b.configuration + " / " + b.target.string ())
        <<     TR_VALUE ("timestamp", ts)
        <<     TR_BUILD_RESULT (b, host, root)
        <<   ~TBODY
        << ~TABLE;
    }

    // Print configurations that are excluded by the package.
    //
    auto excluded = [&pkg] (const bbot::build_config& c, string& reason)
    {
      for (const auto& bc: pkg->build_constraints)
      {
        if (match (bc.config, bc.target, c))
        {
          if (!bc.exclusion)
            return false;

          // Save the first sentence of the exclusion comment, lower-case the
          // first letter if the beginning looks like a word (the second
          // character is the lower-case letter or space).
          //
          reason = bc.comment.substr (0, bc.comment.find ('.'));

          char c;
          size_t n (reason.size ());
          if (n > 0 && alpha (c = reason[0]) && c == ucase (c) &&
              (n == 1 ||
               (alpha (c = reason[1]) && c == lcase (c)) ||
               c == ' '))
            reason[0] = lcase (reason[0]);

          return true;
        }
      }

      return false;
    };

    for (const auto& c: *build_conf_)
    {
      string reason;
      if (excluded (c, reason))
      {
        s << TABLE(CLASS="proplist build")
          <<   TBODY
          <<     TR_VALUE ("config", c.name + " / " + c.target.string ())
          <<     TR_VALUE ("result",
                           !reason.empty ()
                           ? "excluded (" + reason + ')'
                           : "excluded")
          <<   ~TBODY
          << ~TABLE;
      }
    }

    t.commit ();

    s << ~DIV;
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
