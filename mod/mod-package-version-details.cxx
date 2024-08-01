// file      : mod/mod-package-version-details.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-package-version-details.hxx>

#include <libstudxml/serializer.hxx>

#include <odb/session.hxx>
#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <libbutl/filesystem.hxx> // dir_iterator, dir_entry

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

  // Verify that the bindist-url option is specified when necessary.
  //
  if (options_->bindist_root_specified () &&
      !options_->bindist_url_specified ())
    fail << "bindist-url must be specified if bindist-root is specified";

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

  const string title (name + ' ' + sver);
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

  if (const optional<typed_text>& d = pkg->package_description
                                      ? pkg->package_description
                                      : pkg->description)
  {
    const string id ("description");
    const string what (title + " description");

    s << (full
          ? DIV_TEXT (*d,
                      true /* strip_title */,
                      id,
                      what,
                      error)
          : DIV_TEXT (*d,
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
    <<     TR_REPOSITORY (rl, root, tenant);

  if (rl.type () == repository_type::pkg)
  {
    assert (pkg->location);

    s << TR_LINK (rl.url ().string () + '/' + pkg->location->string (),
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
            << A(HREF=u + '/' + p->version.string ()) << *dcon << ~A;
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

    for (const auto& das: ds)
    {
      s << TR(CLASS="depends")
        <<   TH;

      if (das.buildtime)
        s << '*';

      s <<   ~TH
        <<   TD
        <<     SPAN(CLASS="value");

      for (const auto& da: das)
      {
        if (&da != &das[0])
          s << " | ";

        // Should we enclose multiple dependencies into curly braces as in the
        // manifest? Somehow feels redundant here, since there can't be any
        // ambiguity (dependency group version constraint is already punched
        // into the specific dependencies without constraints).
        //
        for (const dependency& d: da)
        {
          if (&d != &da[0])
            s << ' ';

          print_dependency (d);
        }

        if (da.enable)
        {
          s << " ? (";

          if (full)
            s << *da.enable;
          else
            s << "...";

          s << ')';
        }
      }

      s <<     ~SPAN
        <<     SPAN_COMMENT (das.comment)
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

    for (const requirement_alternatives& ras: rm)
    {
      s << TR(CLASS="requires")
        <<   TH;

      if (ras.buildtime)
        s << '*';

      s <<   ~TH
        <<   TD
        <<     SPAN(CLASS="value");

      for (const requirement_alternative& ra: ras)
      {
        if (&ra != &ras[0])
          s << " | ";

        // Should we enclose multiple requirement ids into curly braces as in
        // the manifest? Somehow feels redundant here, since there can't be
        // any ambiguity (requirement group version constraint is already
        // punched into the specific requirements without constraints).
        //
        for (const string& r: ra)
        {
          if (&r != &ra[0])
            s << ' ';

          s << r;
        }

        if (ra.enable)
        {
          if (!ra.simple () || !ra[0].empty ())
            s << ' ';

          s << '?';

          if (!ra.enable->empty ())
          {
            s << " (";

            if (full)
              s << *ra.enable;
            else
              s << "...";

            s << ')';
          }
        }
      }

      s <<     ~SPAN
        <<     SPAN_COMMENT (ras.comment)
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
  auto print_tests = [&pkg,
                      &s,
                      &print_dependency,
                      full] (test_dependency_type dt)
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
          <<   TH;

        if (td.buildtime)
          s << '*';

        s <<   ~TH
          <<   TD
          <<     SPAN(CLASS="value");

        print_dependency (td);

        if (td.enable || td.reflect)
        {
          if (full)
          {
            if (td.enable)
              s << " ? (" << *td.enable << ')';

            if (td.reflect)
              s << ' ' << *td.reflect;
          }
          else
            s << " ...";
        }

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

  if (options_->reviews_url_specified ())
  {
    package_db_->load (*pkg, pkg->reviews_section);

    const optional<reviews_summary>& rvs (pkg->reviews);
    const string& u (options_->reviews_url ());

    s << H3 << "Reviews" << ~H3
      << TABLE(CLASS="proplist", ID="reviews")
      <<   TBODY
      <<     TR_REVIEWS_COUNTER (review_result::fail, rvs, u)
      <<     TR_REVIEWS_COUNTER (review_result::pass, rvs, u)
      <<   ~TBODY
      << ~TABLE;
  }

  bool builds (build_db_ != nullptr && pkg->buildable);

  if (builds)
  {
    package_db_->load (*pkg, pkg->build_section);

    // If all package build configurations has a singe effective build
    // configuration class expression with exactly one underlying class and
    // the class is none, then we just drop the page builds section
    // altogether.
    //
    builds = false;

    for (const package_build_config& pc: pkg->build_configs)
    {
      const build_class_exprs& exprs (pc.effective_builds (pkg->builds));

      if (exprs.size () != 1                       ||
          exprs[0].underlying_classes.size () != 1 ||
          exprs[0].underlying_classes[0] != "none")
      {
        builds = true;
        break;
      }
    }
  }

  shared_ptr<brep::tenant> tn (package_db_->load<brep::tenant> (tenant));

  t.commit ();

  // Display the binary distribution packages for this tenant, package, and
  // version, if present. Print the archive distributions last.
  //
  if (options_->bindist_root_specified ())
  {
    // Collect all the available package configurations by iterating over the
    // <distribution> and <os-release> subdirectories and the <package-config>
    // symlinks in the following filesystem hierarchy:
    //
    // [<tenant>/]<distribution>/<os-release>/<project>/<package>/<version>/<package-config>
    //
    // Note that it is possible that new directories and symlinks are created
    // and/or removed while we iterate over the filesystem entries in the
    // above hierarchy, which may result with system_error exceptions. If that
    // happens, we just ignore such exceptions, trying to collect what we can.
    //
    const dir_path& br (options_->bindist_root ());

    dir_path d (br);

    if (!tenant.empty ())
      d /= tenant;

    // Note that distribution and os_release are simple paths and the
    // config_symlink and config_dir are relative to the bindist root
    // directory.
    //
    struct bindist_config
    {
      dir_path distribution; // debian, fedora, archive
      dir_path os_release;   // fedora37, windows10
      path     symlink;      // .../x86_64, .../x86_64-release
      dir_path directory;    // .../x86_64-2023-05-11T10:13:43Z

      bool
      operator< (const bindist_config& v)
      {
        if (int r = distribution.compare (v.distribution))
          return   distribution.string () == "archive" ? false :
                 v.distribution.string () == "archive" ? true  :
                 r < 0;

        if (int r = os_release.compare (v.os_release))
          return r < 0;

        return symlink < v.symlink;
      }
    };

    vector<bindist_config> configs;

    if (dir_exists (d))
    try
    {
      for (const dir_entry& de: dir_iterator (d, dir_iterator::ignore_dangling))
      {
        if (de.type () != entry_type::directory)
          continue;

        // Distribution directory.
        //
        dir_path dd (path_cast<dir_path> (de.path ()));

        try
        {
          dir_path fdd (d / dd);

          for (const dir_entry& re:
                 dir_iterator (fdd, dir_iterator::ignore_dangling))
          {
            if (re.type () != entry_type::directory)
              continue;

            // OS release directory.
            //
            dir_path rd (path_cast<dir_path> (re.path ()));

            // Package version directory.
            //
            dir_path vd (fdd                               /
                         rd                                /
                         dir_path (pkg->project.string ()) /
                         dir_path (pn.string ())           /
                         dir_path (sver));

            try
            {
              for (const dir_entry& ce:
                     dir_iterator (vd, dir_iterator::ignore_dangling))
              {
                if (ce.ltype () != entry_type::symlink)
                  continue;

                // Skip the "hidden" symlinks which may potentially be used by
                // the upload handlers until they expose the finalized upload
                // directory.
                //
                const path& cl (ce.path ());
                if (cl.string () [0] == '.')
                  continue;

                try
                {
                  path fcl (vd / cl);
                  dir_path cd (path_cast<dir_path> (followsymlink (fcl)));

                  if (cd.sub (br))
                    configs.push_back (
                      bindist_config {dd, rd, fcl.leaf (br), cd.leaf (br)});
                }
                catch (const system_error&) {}
              }
            }
            catch (const system_error&) {}
          }
        }
        catch (const system_error&) {}
      }
    }
    catch (const system_error&) {}

    // Sort and print collected package configurations, if any.
    //
    if (!configs.empty ())
    {
      sort (configs.begin (), configs.end ());

      s << H3 << "Binaries" << ~H3
        << TABLE(ID="binaries")
        <<   TBODY;

      for (const bindist_config& c: configs)
      {
        s << TR(CLASS="binaries")
          <<   TD << SPAN(CLASS="value") << c.distribution << ~SPAN << ~TD
          <<   TD << SPAN(CLASS="value") << c.os_release << ~SPAN << ~TD
          <<   TD
          <<     SPAN(CLASS="value")
          <<       A
          <<         HREF
          <<           options_->bindist_url () << '/' << c.symlink
          <<         ~HREF
          <<         c.symlink.leaf ()
          <<       ~A
          <<       " ("
          <<       A
          <<         HREF
          <<           options_->bindist_url () << '/' << c.directory
          <<         ~HREF
          <<         "snapshot"
          <<       ~A
          <<       ")"
          <<     ~SPAN
          <<   ~TD
          << ~TR;
      }

      s <<   ~TBODY
        << ~TABLE;
    }
  }

  if (builds)
  {
    s << H3 << "Builds" << ~H3
      << DIV(ID="builds");

    auto exclude = [&pkg, this] (const package_build_config& pc,
                                 const build_target_config& tc,
                                 string* rs = nullptr)
    {
      return this->exclude (pc, pkg->builds, pkg->build_constraints, tc, rs);
    };

    timestamp now (system_clock::now ());
    transaction t (build_db_->begin ());

    // Print built and unbuilt package configurations, except those that are
    // hidden or excluded by the package.
    //
    // Query toolchains seen for the package tenant to produce a list of the
    // unbuilt configuration/toolchain combinations.
    //
    vector<pair<string, version>> toolchains;
    {
      using query = query<toolchain>;

      for (auto& t: build_db_->query<toolchain> (
             (!tenant.empty ()
              ? query::build::id.package.tenant == tenant
              : query (true)) +
             "ORDER BY" + query::build::id.toolchain_name +
             order_by_version_desc (query::build::id.toolchain_version,
                                    false /* first */)))
      {
        toolchains.emplace_back (move (t.name), move (t.version));
      }
    }

    // Compose the configuration filtering sub-query and collect unbuilt
    // target configurations, skipping those that are hidden or excluded by
    // the package configurations.
    //
    using query = query<build>;

    query sq (false);
    set<config_toolchain> unbuilt_configs;

    for (const package_build_config& pc: pkg->build_configs)
    {
      for (const auto& bc: *target_conf_map_)
      {
        const build_target_config& tc (*bc.second);

        if (!belongs (tc, "hidden") && !exclude (pc, tc))
        {
          const build_target_config_id& id (bc.first);

          sq = sq || (query::id.target == id.target             &&
                      query::id.target_config_name == id.config &&
                      query::id.package_config_name == pc.name);

          // Note: we will erase built configurations from the unbuilt
          // configurations set later (see below).
          //
          for (const auto& t: toolchains)
            unbuilt_configs.insert (config_toolchain {tc.target,
                                                      tc.name,
                                                      pc.name,
                                                      t.first,
                                                      t.second});
        }
      }
    }

    // Let's not print the package configuration row if the default
    // configuration is the only one.
    //
    bool ppc (pkg->build_configs.size () != 1); // Note: can't be empty.

    // Print the package built configurations in the time-descending order.
    //
    for (auto& b: build_db_->query<build> (
           (query::id.package == pkg->id && query::state != "queued" && sq) +
           "ORDER BY" + query::timestamp + "DESC"))
    {
      string ts (butl::to_string (b.timestamp,
                                  "%Y-%m-%d %H:%M:%S %Z",
                                  true /* special */,
                                  true /* local */) +
                 " (" + butl::to_string (now - b.timestamp, false) + " ago");

      if (tn->archived)
        ts += ", archived";

      ts += ')';

      // @@ Note that here we also load result logs which we don't need.
      //    Probably we should invent some table view to only load operation
      //    names and statuses.
      //
      if (b.state == build_state::built)
        build_db_->load (b, b.results_section);

      s << TABLE(CLASS="proplist build")
        <<   TBODY
        <<     TR_VALUE ("toolchain",
                         b.toolchain_name + '-' +
                         b.toolchain_version.string ())
        <<     TR_VALUE ("target", b.target.string ())
        <<     TR_VALUE ("tgt config", b.target_config_name);

      if (ppc)
        s <<   TR_VALUE ("pkg config", b.package_config_name);

      s  <<    TR_VALUE ("timestamp", ts);

      if (b.interactive) // Note: can only be present for the building state.
        s <<   TR_VALUE ("login", *b.interactive);

      s <<     TR_BUILD_RESULT (b, tn->archived, host, root)
        <<   ~TBODY
        << ~TABLE;

      // While at it, erase the built configuration from the unbuilt
      // configurations set.
      //
      unbuilt_configs.erase (config_toolchain {b.target,
                                               b.target_config_name,
                                               b.package_config_name,
                                               b.toolchain_name,
                                               b.toolchain_version});
    }

    // Print the package unbuilt configurations with the following sort
    // priority:
    //
    // 1: toolchain name
    // 2: toolchain version (descending)
    // 3: target
    // 4: target configuration name
    // 5: package configuration name
    //
    for (const auto& ct: unbuilt_configs)
    {
      s << TABLE(CLASS="proplist build")
        <<   TBODY
        <<     TR_VALUE ("toolchain",
                         ct.toolchain_name + '-' +
                         ct.toolchain_version.string ())
        <<     TR_VALUE ("target", ct.target.string ())
        <<     TR_VALUE ("tgt config", ct.target_config);

      if (ppc)
        s <<   TR_VALUE ("pkg config", ct.package_config);

      s <<     TR_VALUE ("result", "unbuilt")
        <<   ~TBODY
        << ~TABLE;
    }

    // Print the package build exclusions that belong to the 'default' class,
    // unless the package is built interactively (normally for a single
    // configuration).
    //
    if (!tn->interactive)
    {
      for (const package_build_config& pc: pkg->build_configs)
      {
        for (const auto& tc: *target_conf_)
        {
          string reason;
          if (belongs (tc, "default") && exclude (pc, tc, &reason))
          {
            s << TABLE(CLASS="proplist build")
              <<   TBODY
              <<     TR_VALUE ("target", tc.target.string ())
              <<     TR_VALUE ("tgt config", tc.name);

            if (ppc)
              s <<   TR_VALUE ("pkg config", pc.name);

            s <<     TR_VALUE ("result",
                               !reason.empty ()
                               ? "excluded (" + reason + ')'
                               : "excluded")
              <<   ~TBODY
              << ~TABLE;
          }
        }
      }
    }

    t.commit ();

    s << ~DIV;
  }

  if (const optional<typed_text>& c = pkg->changes)
  {
    const string id ("changes");
    const string what (title + " changes");

    s << H3 << "Changes" << ~H3
      << (full
          ? DIV_TEXT (*c,
                      false /* strip_title */,
                      id,
                      what,
                      error)
          : DIV_TEXT (*c,
                      false /* strip_title */,
                      options_->package_changes (),
                      url (!full, id),
                      id,
                      what,
                      error));
  }

  s <<     ~DIV
    <<   ~BODY
    << ~HTML;

  return true;
}
