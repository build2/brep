// file      : mod/mod-package-version-details.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-package-version-details.hxx>

#include <libstudxml/serializer.hxx>

#include <odb/session.hxx>
#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <web/server/module.hxx>
#include <web/server/mime-url-encoding.hxx>

#include <web/xhtml/serialization.hxx>

#include <libbrep/build.hxx>
#include <libbrep/build-odb.hxx>
#include <libbrep/package.hxx>
#include <libbrep/package-odb.hxx>

#include <mod/page.hxx>
#include <mod/module-options.hxx>

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
      build_config_module (r),
      options_ (r.initialized_ ? r.options_ : nullptr)
{
}

void brep::package_version_details::
init (scanner& s)
{
  HANDLER_DIAG;

  options_ = make_shared<options::package_version_details> (
    s, unknown_mode::fail, unknown_mode::fail);

  database_module::init (static_cast<const options::package_db&> (*options_),
                         options_->package_db_retry ());

  if (options_->build_config_specified ())
  {
    database_module::init (static_cast<const options::build_db&> (*options_),
                           options_->build_db_retry ());

    build_config_module::init (*options_);
  }

  if (options_->root ().empty ())
    options_->root (dir_path ("/"));
}

bool brep::package_version_details::
handle (request& rq, response& rs)
{
  using namespace web;
  using namespace web::xhtml;
  using brep::version; // Not to confuse with module::version.

  HANDLER_DIAG;

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

  assert (i != rq.path ().rend ());

  package_name pn;

  try
  {
    pn = package_name (*i);
  }
  catch (const invalid_argument& )
  {
    throw invalid_request (400, "invalid package name format");
  }

  params::package_version_details params;
  bool full;

  try
  {
    name_value_scanner s (rq.parameters (1024));
    params = params::package_version_details (
      s, unknown_mode::fail, unknown_mode::fail);

    full = params.form () == page_form::full;
  }
  catch (const cli::exception& e)
  {
    throw invalid_request (400, e.what ());
  }

  const string& sver (ver.string ());

  auto url = [&sver] (bool f = false, const string& a = "") -> string
  {
    string u (sver);

    if (f)           { u += "?f=full"; }
    if (!a.empty ()) { u += '#' + a; }
    return u;
  };

  bool not_found (false);
  shared_ptr<package> pkg;

  session sn;
  transaction t (package_db_->begin ());

  try
  {
    pkg = package_db_->load<package> (package_id (tenant, pn, ver));

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
    throw invalid_request (
      404, "Package " + pn.string () + '/' + sver + " not (yet) found");

  const string& name (pkg->name.string ());

  const string title (name + " " + sver);
  xml::serializer s (rs.content (), title);

  s << HTML
    <<   HEAD
    <<     TITLE << title << ~TITLE
    <<     CSS_LINKS (path ("package-version-details.css"), root)
    <<   ~HEAD
    <<   BODY
    <<     DIV_HEADER (options_->logo (), options_->menu (), root, tenant)
    <<     DIV(ID="content");

  if (full)
    s << CLASS("full");

  s <<       DIV(ID="heading")
    <<         H1
    <<           A(HREF=tenant_dir (root, tenant) /
                   path (mime_url_encode (name, false)))
    <<             name
    <<           ~A
    <<           "/"
    <<           A(HREF=url ()) << sver << ~A
    <<         ~H1
    <<         A(HREF=url (!full)) << (full ? "[brief]" : "[full]") << ~A
    <<       ~DIV;

  s << H2 << pkg->summary << ~H2;

  if (const optional<string>& d = pkg->description)
  {
    const string id ("description");
    const string what (title + " description");

    s << (full
          ? DIV_TEXT (*d, *
                      pkg->description_type,
                      true /* strip_title */,
                      id,
                      what,
                      error)
          : DIV_TEXT (*d,
                      *pkg->description_type,
                      true /* strip_title */,
                      options_->package_description (),
                      url (!full, id),
                      id,
                      what,
                      error));
  }

  const repository_location& rl (pkg->internal_repository.load ()->location);

  s << TABLE(CLASS="proplist", ID="version")
    <<   TBODY

    // Repeat version here since it can be cut out in the header.
    //
    <<     TR_VERSION (pkg->version, pkg->upstream_version)

    <<     TR_PRIORITY (pkg->priority)
    <<     TR_LICENSES (pkg->license_alternatives)
    <<     TR_REPOSITORY (rl.canonical_name (), root, tenant)
    <<     TR_LOCATION (rl);

  if (rl.type () == repository_type::pkg)
  {
    assert (pkg->location);

    s << TR_LINK (rl.url ().string () + "/" + pkg->location->string (),
                  pkg->location->leaf ().string (),
                  "download");
  }

  if (pkg->fragment)
    s << TR_VALUE ("fragment", *pkg->fragment);

  if (pkg->sha256sum)
    s << TR_SHA256SUM (*pkg->sha256sum);

  s <<   ~TBODY
    << ~TABLE

    << TABLE(CLASS="proplist", ID="package")
    <<   TBODY
    <<     TR_PROJECT (pkg->project, root, tenant);

  const auto& u (pkg->url);

  if (u)
    s << TR_URL (*u);

  if (pkg->doc_url)
    s << TR_URL (*pkg->doc_url, "doc-url");

  if (pkg->src_url)
    s << TR_URL (*pkg->src_url, "src-url");

  const auto& pu (pkg->package_url);
  if (pu && pu != u)
    s << TR_URL (*pu, "package-url");

  const auto& em (pkg->email);

  if (em)
    s << TR_EMAIL (*em);

  const auto& pe (pkg->package_email);
  if (pe && pe != em)
    s << TR_EMAIL (*pe, "package-email");

  s <<     TR_TOPICS (pkg->topics, root, tenant)
    <<   ~TBODY
    << ~TABLE;

  auto print_dependency = [&s, &root, this] (const dependency& d)
  {
    const auto& dcon (d.constraint);
    const package_name& dname (d.name);

    // Try to display the dependency as a link if it is resolved.
    // Otherwise display it as a plain text.
    //
    if (d.package != nullptr)
    {
      shared_ptr<package> p (d.package.load ());
      assert (p->internal () || !p->other_repositories.empty ());

      shared_ptr<repository> r (p->internal ()
                                ? p->internal_repository.load ()
                                : p->other_repositories[0].load ());

      string ename (mime_url_encode (dname.string (), false));

      if (r->interface_url)
      {
        string u (*r->interface_url + ename);
        s << A(HREF=u) << dname << ~A;

        if (dcon)
          s << ' '
            << A(HREF=u + "/" + p->version.string ()) << *dcon << ~A;
      }
      else if (p->internal ())
      {
        dir_path u (tenant_dir (root, tenant) / dir_path (ename));
        s << A(HREF=u) << dname << ~A;

        if (dcon)
          s << ' '
            << A(HREF=u / path (p->version.string ())) << *dcon << ~A;
      }
      else
        // Display the dependency as a plain text if no repository URL
        // available.
        //
        s << d;
    }
    else
      s << d;
  };

  const auto& ds (pkg->dependencies);
  if (!ds.empty ())
  {
    s << H3 << "Depends (" << ds.size () << ")" << ~H3
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

        print_dependency (d);
      }

      s <<     ~SPAN
        <<     SPAN_COMMENT (da.comment)
        <<   ~TD
        << ~TR;
    }

    s <<   ~TBODY
      << ~TABLE;
  }

  const auto& rm (pkg->requirements);
  if (!rm.empty ())
  {
    s << H3 << "Requires (" << rm.size () << ")" << ~H3
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

  // Print the test dependencies grouped by types as the separate blocks.
  //
  // Print test dependencies of the specific type.
  //
  auto print_tests = [&pkg, &s, &print_dependency] (test_dependency_type dt)
  {
    string id;

    bool first (true);
    for (const test_dependency& td: pkg->tests)
    {
      if (td.type == dt)
      {
        // Print the table header if this is a first test dependency.
        //
        if (first)
        {
          id = to_string (dt);

          // Capitalize the heading.
          //
          string heading (id);
          heading[0] = ucase (id[0]);

          s << H3 << heading << ~H3
            << TABLE(CLASS="proplist", ID=id)
            <<   TBODY;

          first = false;
        }

        s << TR(CLASS=id)
          <<   TD
          <<     SPAN(CLASS="value");

        print_dependency (td);

        s <<     ~SPAN
          <<   ~TD
          << ~TR;
      }
    }

    // Print the table closing tags if it was printed.
    //
    if (!first)
    {
      s <<   ~TBODY
        << ~TABLE;
    }
  };

  print_tests (test_dependency_type::tests);
  print_tests (test_dependency_type::examples);
  print_tests (test_dependency_type::benchmarks);

  bool builds (build_db_ != nullptr && pkg->buildable);

  if (builds)
  {
    package_db_->load (*pkg, pkg->build_section);

    // If the package has a singe build configuration class expression with
    // exactly one underlying class and the class is none, then we just drop
    // the page builds section altogether.
    //
    if (pkg->builds.size () == 1)
    {
      const build_class_expr& be (pkg->builds[0]);

      builds = be.underlying_classes.size () != 1 ||
               be.underlying_classes[0] != "none";
    }
  }

  shared_ptr<brep::tenant> tn (package_db_->load<brep::tenant> (tenant));

  t.commit ();

  if (builds)
  {
    using bbot::build_config;

    s << H3 << "Builds" << ~H3
      << DIV(ID="builds");

    auto exclude = [&pkg, this] (const build_config& cfg,
                                 string* reason = nullptr)
    {
      return this->exclude (pkg->builds, pkg->build_constraints, cfg, reason);
    };

    timestamp now (system_clock::now ());
    transaction t (build_db_->begin ());

    // Print built and unbuilt package configurations, except those that are
    // hidden or excluded by the package.
    //
    // Query toolchains seen for the package tenant to produce a list of the
    // unbuilt configuration/toolchain combinations.
    //
    // Note that it only make sense to print those unbuilt configurations that
    // may still be built. That's why we leave the toolchains list empty if
    // the package tenant is achieved.
    //
    vector<pair<string, version>> toolchains;

    if (!tn->archived)
    {
      using query = query<toolchain>;

      for (auto& t: build_db_->query<toolchain> (
             (!tenant.empty ()
              ? query::build::id.package.tenant == tenant
              : query (true)) +
             "ORDER BY" + query::build::id.toolchain_name +
             order_by_version_desc (query::build::id.toolchain_version,
                                    false /* first */)))
        toolchains.emplace_back (move (t.name), move (t.version));
    }

    // Collect configuration names and unbuilt configurations, skipping those
    // that are hidden or excluded by the package.
    //
    cstrings conf_names;
    set<config_toolchain> unbuilt_configs;

    for (const auto& c: *build_conf_map_)
    {
      const build_config& cfg (*c.second);

      if (belongs (cfg, "all") && !exclude (cfg))
      {
        conf_names.push_back (c.first);

        // Note: we will erase built configurations from the unbuilt
        // configurations set later (see below).
        //
        for (const auto& t: toolchains)
          unbuilt_configs.insert ({cfg.name, t.first, t.second});
      }
    }

    // Print the package built configurations in the time-descending order.
    //
    using query = query<build>;

    for (auto& b: build_db_->query<build> (
           (query::id.package == pkg->id &&

            query::id.configuration.in_range (conf_names.begin (),
                                              conf_names.end ())) +

           "ORDER BY" + query::timestamp + "DESC"))
    {
      string ts (butl::to_string (b.timestamp,
                                  "%Y-%m-%d %H:%M:%S %Z",
                                  true /* special */,
                                  true /* local */) +
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
        <<     TR_VALUE ("timestamp", ts);

      if (b.interactive) // Note: can only be present for the building state.
        s <<   TR_VALUE ("login", *b.interactive);

      s <<     TR_BUILD_RESULT (b, host, root)
        <<   ~TBODY
        << ~TABLE;

      // While at it, erase the built configuration from the unbuilt
      // configurations set.
      //
      unbuilt_configs.erase ({b.id.configuration,
                              b.toolchain_name,
                              b.toolchain_version});
    }

    // Print the package unbuilt configurations with the following sort
    // priority:
    //
    // 1: toolchain name
    // 2: toolchain version (descending)
    // 3: configuration name
    //
    for (const auto& ct: unbuilt_configs)
    {
      auto i (build_conf_map_->find (ct.configuration.c_str ()));
      assert (i != build_conf_map_->end ());

      s << TABLE(CLASS="proplist build")
        <<   TBODY
        <<     TR_VALUE ("toolchain",
                         ct.toolchain_name + '-' +
                         ct.toolchain_version.string ())
        <<     TR_VALUE ("config",
                         ct.configuration + " / " +
                         i->second->target.string ())
        <<     TR_VALUE ("result", "unbuilt")
        <<   ~TBODY
        << ~TABLE;
    }

    // Print the package build exclusions that belong to the 'default' class,
    // unless the package is built interactively (normally for a single
    // configuration).
    //
    if (!tn->interactive)
    {
      for (const auto& c: *build_conf_)
      {
        string reason;
        if (belongs (c, "default") && exclude (c, &reason))
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
    }

    t.commit ();

    s << ~DIV;
  }

  const string& ch (pkg->changes);

  if (!ch.empty ())
  {
    const string id ("changes");

    s << H3 << "Changes" << ~H3
      << (full
          ? PRE_TEXT (ch, id)
          : PRE_TEXT (ch,
                      options_->package_changes (),
                      url (!full, "changes"),
                      id));
  }

  s <<     ~DIV
    <<   ~BODY
    << ~HTML;

  return true;
}
