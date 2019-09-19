// file      : mod/mod-builds.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-builds.hxx>

#include <set>
#include <algorithm> // find_if()

#include <libstudxml/serializer.hxx>

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <libbutl/timestamp.mxx>  // to_string()
#include <libbutl/filesystem.mxx> // path_match()

#include <libbbot/manifest.hxx> // to_result_status(), to_string(result_status)

#include <web/xhtml.hxx>
#include <web/module.hxx>
#include <web/mime-url-encoding.hxx>

#include <libbrep/build.hxx>
#include <libbrep/build-odb.hxx>
#include <libbrep/build-package.hxx>
#include <libbrep/build-package-odb.hxx>

#include <mod/page.hxx>
#include <mod/options.hxx>

using namespace std;
using namespace butl;
using namespace bbot;
using namespace web;
using namespace odb::core;
using namespace brep::cli;

// While currently the user-defined copy constructor is not required (we don't
// need to deep copy nullptr's), it is a good idea to keep the placeholder
// ready for less trivial cases.
//
brep::builds::
builds (const builds& r)
    : database_module (r),
      build_config_module (r),
      options_ (r.initialized_ ? r.options_ : nullptr)
{
}

void brep::builds::
init (scanner& s)
{
  HANDLER_DIAG;

  options_ = make_shared<options::builds> (
    s, unknown_mode::fail, unknown_mode::fail);

  if (options_->build_config_specified ())
  {
    database_module::init (*options_, options_->build_db_retry ());
    build_config_module::init (*options_);
  }

  if (options_->root ().empty ())
    options_->root (dir_path ("/"));
}

// Transform the wildcard to the LIKE-pattern.
//
static string
transform (const string& s)
{
  if (s.empty ())
    return "%";

  string r;
  for (char c: s)
  {
    switch (c)
    {
    case '*': c = '%'; break;
    case '?': c = '_'; break;
    case '\\':
    case '%':
    case '_': r += '\\'; break;
    }

    r += c;
  }

  return r;
}

template <typename T>
static inline query<T>
build_query (const brep::cstrings* configs,
             const brep::params::builds& params,
             const brep::optional<brep::string>& tenant,
             const brep::optional<bool>& archived)
{
  using namespace brep;
  using query = query<T>;
  using qb = typename query::build;

  query q (configs != nullptr
           ? qb::id.configuration.in_range (configs->begin (), configs->end ())
           : query (true));

  const auto& pid (qb::id.package);

  if (tenant)
    q = q && pid.tenant == *tenant;

  if (archived)
    q = q && query::build_tenant::archived == *archived;

  // Note that there is no error reported if the filter parameters parsing
  // fails. Instead, it is considered that no package builds match such a
  // query.
  //
  try
  {
    // Package name.
    //
    if (!params.name ().empty ())
      q = q && pid.name.like (package_name (transform (params.name ()),
                                            package_name::raw_string));

    // Package version.
    //
    if (!params.version ().empty () && params.version () != "*")
    {
      // May throw invalid_argument.
      //
      version v (params.version (), false /* fold_zero_revision */);

      q = q && compare_version_eq (pid.version,
                                   canonical_version (v),
                                   v.revision.has_value ());
    }

    // Build toolchain name/version.
    //
    const string& tc (params.toolchain ());

    if (tc != "*")
    {
      size_t p (tc.find ('-'));
      if (p == string::npos) // Invalid format.
        throw invalid_argument ("");

      // Note that the toolchain version is selected from the list and denotes
      // the exact version revision, so an absent and zero revisions have the
      // same semantics and the zero revision is folded.
      //
      string  tn (tc, 0, p);
      version tv (string (tc, p + 1)); // May throw invalid_argument.

      q = q                           &&
          qb::id.toolchain_name == tn &&
          compare_version_eq (qb::id.toolchain_version,
                              canonical_version (tv),
                              true /* revision */);
    }

    // Build configuration name.
    //
    if (!params.configuration ().empty ())
      q = q && qb::id.configuration.like (transform (params.configuration ()));

    // Build machine name.
    //
    if (!params.machine ().empty ())
      q = q && qb::machine.like (transform (params.machine ()));

    // Build target.
    //
    if (!params.target ().empty ())
      q = q && qb::target.like (transform (params.target ()));

    // Build result.
    //
    const string& rs (params.result ());

    if (rs != "*")
    {
      if (rs == "pending")
        q = q && qb::force != "unforced";
      else if (rs == "building")
        q = q && qb::state == "building";
      else
      {
        query sq (qb::status == rs);
        result_status st (to_result_status(rs)); // May throw invalid_argument.

        if (st != result_status::success)
        {
          auto next = [&st] () -> bool
          {
            if (st == result_status::abnormal)
              return false;

            st = static_cast<result_status> (static_cast<uint8_t> (st) + 1);
            return true;
          };

          while (next ())
            sq = sq || qb::status == to_string (st);
        }

        // Note that the result status may present for the building state as
        // well (rebuild).
        //
        q = q && qb::state == "built" && sq;
      }
    }
  }
  catch (const invalid_argument&)
  {
    return query (false);
  }

  return q;
}

template <typename T>
static inline query<T>
package_query (const brep::params::builds& params,
               const brep::optional<brep::string>& tenant,
               const brep::optional<bool>& archived)
{
  using namespace brep;
  using query = query<T>;
  using qp = typename query::build_package;

  query q (true);

  if (tenant)
    q = q && qp::id.tenant == *tenant;

  if (archived)
    q = q && query::build_tenant::archived == *archived;

  // Note that there is no error reported if the filter parameters parsing
  // fails. Instead, it is considered that no packages match such a query.
  //
  try
  {
    // Package name.
    //
    if (!params.name ().empty ())
      q = q && qp::id.name.like (
        package_name (transform (params.name ()), package_name::raw_string));

    // Package version.
    //
    if (!params.version ().empty () && params.version () != "*")
    {
      // May throw invalid_argument.
      //
      version v (params.version (), false /* fold_zero_revision */);

      q = q && compare_version_eq (qp::id.version,
                                   canonical_version (v),
                                   v.revision.has_value ());
    }
  }
  catch (const invalid_argument&)
  {
    return query (false);
  }

  return q;
}

template <typename T, typename ID>
static inline query<T>
package_id_eq (const ID& x, const brep::package_id& y)
{
  using query = query<T>;
  const auto& qv (x.version);

  return
    x.tenant == query::_ref (y.tenant)                                  &&
    x.name == query::_ref (y.name)                                      &&
    qv.epoch == query::_ref (y.version.epoch)                           &&
    qv.canonical_upstream == query::_ref (y.version.canonical_upstream) &&
    qv.canonical_release == query::_ref (y.version.canonical_release)   &&
    qv.revision == query::_ref (y.version.revision);
}

static const vector<pair<string, string>> build_results ({
    {"unbuilt",  "<unbuilt>"},
    {"*",        "*"},
    {"pending",  "pending"},
    {"building", "building"},
    {"success",  "success"},
    {"warning",  "warning"},
    {"error",    "error"},
    {"abort",    "abort"},
    {"abnormal", "abnormal"}});

bool brep::builds::
handle (request& rq, response& rs)
{
  using brep::version;
  using namespace web::xhtml;

  HANDLER_DIAG;

  if (build_db_ == nullptr)
    throw invalid_request (501, "not implemented");

  const size_t page_configs (options_->build_page_entries ());
  const string& host (options_->host ());
  const dir_path& root (options_->root ());
  const string& tenant_name (options_->tenant_name ());

  params::builds params;

  try
  {
    name_value_scanner s (rq.parameters (8 * 1024));
    params = params::builds (s, unknown_mode::fail, unknown_mode::fail);
  }
  catch (const cli::exception& e)
  {
    throw invalid_request (400, e.what ());
  }

  // Override the name parameter for the old URL (see options.cli for details).
  //
  if (params.name_legacy_specified ())
    params.name (params.name_legacy ());

  const char* title ("Builds");

  xml::serializer s (rs.content (), title);

  s << HTML
    <<   HEAD
    <<     TITLE << title << ~TITLE
    <<     CSS_LINKS (path ("builds.css"), root)
    //
    // This hack is required to avoid the "flash of unstyled content", which
    // happens due to the presence of the autofocus attribute in the input
    // element of the filter form. The problem appears in Firefox and has a
    // (4-year old, at the time of this writing) bug report:
    //
    // https://bugzilla.mozilla.org/show_bug.cgi?id=712130.
    //
    <<     SCRIPT << " " << ~SCRIPT
    <<   ~HEAD
    <<   BODY
    <<     DIV_HEADER (options_->logo (), options_->menu (), root, tenant)
    <<     DIV(ID="content");

  // If the tenant is empty then we are in the global view and will display
  // builds from all the tenants.
  //
  optional<string> tn;
  if (!tenant.empty ())
    tn = tenant;

  // Return the list of distinct toolchain name/version pairs. The build db
  // transaction must be started.
  //
  using toolchains = vector<pair<string, version>>;

  auto query_toolchains = [this, &tn] () -> toolchains
  {
    using query = query<toolchain>;

    toolchains r;
    for (auto& t: build_db_->query<toolchain> (
           (tn ? query::build::id.package.tenant == *tn : query (true)) +
           "ORDER BY" + query::build::id.toolchain_name +
           order_by_version_desc (query::build::id.toolchain_version,
                                  false /* first */)))
      r.emplace_back (move (t.name), move (t.version));

    return r;
  };

  auto print_form = [&s, &params, this] (const toolchains& toolchains,
                                         size_t build_count)
  {
    // Print the package builds filter form on the first page only.
    //
    if (params.page () == 0)
    {
      // Populate the toolchains list with the distinct list of toolchain
      // name/version pairs from all the existing package builds. Make sure
      // the selected toolchain is still present in the database. Otherwise
      // fallback to the * wildcard selection.
      //
      string ctc ("*");
      vector<pair<string, string>> toolchain_opts ({{"*", "*"}});
      {
        for (const auto& t: toolchains)
        {
          string tc (t.first + '-' + t.second.string ());
          toolchain_opts.emplace_back (tc, tc);

          if (tc == params.toolchain ())
            ctc = move (tc);
        }
      }

      // The 'action' attribute is optional in HTML5. While the standard
      // doesn't specify browser behavior explicitly for the case the
      // attribute is omitted, the only reasonable behavior is to default it
      // to the current document URL. Note that we specify the function name
      // using the "hidden" <input/> element since the action url must not
      // contain the query part.
      //
      s << FORM
        <<   TABLE(ID="filter", CLASS="proplist")
        <<     TBODY
        <<       TR_INPUT  ("name", "builds", params.name (), "*", true)
        <<       TR_INPUT  ("version", "pv", params.version (), "*")
        <<       TR_SELECT ("toolchain", "tc", ctc, toolchain_opts)

        <<       TR(CLASS="config")
        <<         TH << "config" << ~TH
        <<         TD
        <<           *INPUT(TYPE="text",
                            NAME="cf",
                            VALUE=params.configuration (),
                            PLACEHOLDER="*",
                            LIST="configs")
        <<          DATALIST(ID="configs")
        <<            *OPTION(VALUE="*");

      for (const auto& c: *build_conf_names_)
        s << *OPTION(VALUE=c);

      s <<          ~DATALIST
        <<         ~TD
        <<       ~TR

        <<       TR_INPUT  ("machine", "mn", params.machine (), "*")
        <<       TR_INPUT  ("target", "tg", params.target (), "*")
        <<       TR_SELECT ("result", "rs", params.result (), build_results)
        <<     ~TBODY
        <<   ~TABLE
        <<   TABLE(CLASS="form-table")
        <<     TBODY
        <<       TR
        <<         TD(ID="build-count")
        <<           DIV_COUNTER (build_count, "Build", "Builds")
        <<         ~TD
        <<         TD(ID="filter-btn")
        <<           *INPUT(TYPE="submit", VALUE="Filter")
        <<         ~TD
        <<       ~TR
        <<     ~TBODY
        <<   ~TABLE
        << ~FORM;
    }
    else
      s << DIV_COUNTER (build_count, "Build", "Builds");
  };

  // We will not display hidden configurations, unless the configuration is
  // specified explicitly.
  //
  bool exclude_hidden (
    params.configuration ().empty () ||
    params.configuration ().find_first_of ("*?") != string::npos);

  cstrings conf_names;

  if (exclude_hidden)
  {
    for (const auto& c: *build_conf_map_)
    {
      if (belongs (*c.second, "all"))
        conf_names.push_back (c.first);
    }
  }
  else
    conf_names = *build_conf_names_;

  size_t count;
  size_t page (params.page ());

  if (params.result () != "unbuilt") // Print package build configurations.
  {
    // It seems impossible to filter out the package-excluded configuration
    // builds via the database query. Thus, we will traverse through builds
    // that pass the form filter and match them against expressions and
    // constraints of a package they are builds of.
    //
    // We will calculate the total builds count and cache build objects for
    // printing on the same pass. Note that we need to print the count before
    // printing the builds.
    //
    count = 0;
    vector<shared_ptr<build>> builds;
    builds.reserve (page_configs);

    // Prepare the package build prepared query.
    //
    using query = query<package_build>;
    using prep_query = prepared_query<package_build>;

    query q (build_query<package_build> (
               &conf_names, params, tn, nullopt /* archived */));

    // Specify the portion. Note that we will be querying builds in chunks,
    // not to hold locks for too long.
    //
    // Also note that for each build we also load the corresponding
    // package. Nevertheless, we use a fairly large portion to speed-up the
    // builds traversal but also cache the package objects (see below).
    //
    size_t offset (0);

    // Print package build configurations ordered by the timestamp (later goes
    // first).
    //
    q += "ORDER BY" + query::build::timestamp + "DESC" +
      "OFFSET" + query::_ref (offset) + "LIMIT 500";

    connection_ptr conn (build_db_->connection ());

    prep_query pq (
      conn->prepare_query<package_build> ("mod-builds-query", q));

    // Note that we can't skip the proper number of builds in the database
    // query for a page numbers greater than one. So we will query builds from
    // the very beginning and skip the appropriate number of them while
    // iterating through the query result.
    //
    size_t skip (page * page_configs);
    size_t print (page_configs);

    // Cache the build package objects that would otherwise be loaded multiple
    // times for different configuration/toolchain combinations. Note that the
    // build package is a subset of the package object and normally has a
    // small memory footprint.
    //
    session sn;

    for (bool ne (true); ne; )
    {
      transaction t (conn->begin ());

      // Query package builds (and cache the result).
      //
      auto bs (pq.execute ());

      if ((ne = !bs.empty ()))
      {
        offset += bs.size ();

        // Iterate over builds and cache build objects that should be printed.
        // Skip the appropriate number of them (for page number greater than
        // one).
        //
        for (auto& pb: bs)
        {
          shared_ptr<build>& b (pb.build);

          auto i (build_conf_map_->find (b->configuration.c_str ()));
          assert (i != build_conf_map_->end ());

          // Match the configuration against the package build
          // expressions/constraints.
          //
          shared_ptr<build_package> p (
            build_db_->load<build_package> (b->id.package));

          if (!exclude (p->builds, p->constraints, *i->second))
          {
            if (skip != 0)
              --skip;
            else if (print != 0)
            {
              // As we query builds in multiple transactions we may see the
              // same build multiple times. Let's skip the duplicates. Note:
              // we don't increment the counter in this case.
              //
              if (find_if (builds.begin (),
                           builds.end (),
                           [&b] (const shared_ptr<build>& pb)
                           {
                             return b->id == pb->id;
                           }) != builds.end ())
                continue;

              if (b->state == build_state::built)
              {
                build_db_->load (*b, b->results_section);

                // Let's clear unneeded result logs for builds being cached.
                //
                for (operation_result& r: b->results)
                  r.log.clear ();
              }

              builds.push_back (move (b));

              --print;
            }

            ++count;
          }
        }
      }

      // Print the filter form after the build count is calculated. Note:
      // query_toolchains() must be called inside the build db transaction.
      //
      else
        print_form (query_toolchains (), count);

      t.commit ();
    }

    // Finally, print the cached package build configurations.
    //
    timestamp now (system_clock::now ());

    // Enclose the subsequent tables to be able to use nth-child CSS selector.
    //
    s << DIV;
    for (const shared_ptr<build>& pb: builds)
    {
      const build& b (*pb);

      string ts (butl::to_string (b.timestamp,
                                  "%Y-%m-%d %H:%M:%S %Z",
                                  true /* special */,
                                  true /* local */) +
                 " (" + butl::to_string (now - b.timestamp, false) + " ago)");

      s << TABLE(CLASS="proplist build")
        <<   TBODY
        <<     TR_NAME (b.package_name, string (), root, b.tenant)
        <<     TR_VERSION (b.package_name, b.package_version, root, b.tenant)
        <<     TR_VALUE ("toolchain",
                         b.toolchain_name + '-' +
                         b.toolchain_version.string ())
        <<     TR_VALUE ("config", b.configuration)
        <<     TR_VALUE ("machine", b.machine)
        <<     TR_VALUE ("target", b.target.string ())
        <<     TR_VALUE ("timestamp", ts)
        <<     TR_BUILD_RESULT (b, host, root);

      // In the global view mode add the tenant builds link. Note that the
      // global view (and the link) makes sense only in the multi-tenant mode.
      //
      if (!tn && !b.tenant.empty ())
        s << TR_TENANT (tenant_name, "builds", root, b.tenant);

      s <<   ~TBODY
        << ~TABLE;
    }
    s << ~DIV;
  }
  else // Print unbuilt package configurations.
  {
    // Parameters to use for package build configurations queries. Note that
    // we cleanup the machine and the result filter arguments, as they are
    // irrelevant for unbuilt configurations.
    //
    params::builds bld_params (params);
    bld_params.machine ().clear ();
    bld_params.result () = "*";

    // Query toolchains, filter build configurations and toolchains, and
    // create the set of configuration/toolchain combinations, that we will
    // print for packages. Also calculate the number of unbuilt package
    // configurations.
    //
    toolchains toolchains;

    // Note that config_toolchains contains shallow references to the
    // toolchain names and versions.
    //
    set<config_toolchain> config_toolchains;
    {
      transaction t (build_db_->begin ());
      toolchains = query_toolchains ();

      string tc_name;
      version tc_version;
      const string& tc (params.toolchain ());

      if (tc != "*")
      try
      {
        size_t p (tc.find ('-'));
        if (p == string::npos)         // Invalid format.
          throw invalid_argument ("");

        tc_name.assign (tc, 0, p);

        // May throw invalid_argument.
        //
        // Note that an absent and zero revisions have the same semantics,
        // so the zero revision is folded (see above for details).
        //
        tc_version = version (string (tc, p + 1));
      }
      catch (const invalid_argument&)
      {
        // This is unlikely to be the user fault, as he selects the toolchain
        // from the list.
        //
        throw invalid_request (400, "invalid toolchain");
      }

      const string& pc (params.configuration ());
      const string& tg (params.target ());
      vector<const build_config*> configs;

      for (const auto& c: *build_conf_)
      {
        if ((pc.empty () || path_match (pc, c.name)) && // Filter by name.

            // Filter by target.
            //
            (tg.empty () || path_match (tg, c.target.string ())) &&

            (!exclude_hidden || belongs (c, "all"))) // Filter hidden.
        {
          configs.push_back (&c);

          for (const auto& t: toolchains)
          {
            // Filter by toolchain.
            //
            if (tc == "*" || (t.first == tc_name && t.second == tc_version))
              config_toolchains.insert ({c.name, t.first, t.second});
          }
        }
      }

      // Calculate the number of unbuilt package configurations as a
      // difference between the maximum possible number of unbuilt
      // configurations and the number of existing package builds.
      //
      // Note that we also need to deduct the package-excluded configurations
      // count from the maximum possible number of unbuilt configurations. The
      // only way to achieve this is to traverse through the packages and
      // match their build expressions/constraints against our configurations.
      //
      // Also note that some existing builds can now be excluded by packages
      // due to the build configuration target or class set change. We should
      // deduct such builds count from the number of existing package builds.
      //
      size_t nmax (
        config_toolchains.size () *
        build_db_->query_value<buildable_package_count> (
          package_query<buildable_package_count> (
            params, tn, false /* archived */)));

      size_t ncur = build_db_->query_value<package_build_count> (
        build_query<package_build_count> (
          &conf_names, bld_params, tn, false /* archived */));

      // From now we will be using specific package name and version for each
      // build database query.
      //
      bld_params.name ().clear ();
      bld_params.version ().clear ();

      if (!config_toolchains.empty ())
      {
        // Prepare the build count prepared query.
        //
        // For each package-excluded configuration we will query the number of
        // existing builds.
        //
        using bld_query = query<package_build_count>;
        using prep_bld_query = prepared_query<package_build_count>;

        package_id id;
        string config;

        const auto& bid (bld_query::build::id);

        bld_query bq (
          package_id_eq<package_build_count> (bid.package, id) &&
          bid.configuration == bld_query::_ref (config)        &&

          // Note that the query already constrains configurations via the
          // configuration name and the tenant via the build package id.
          //
          build_query<package_build_count> (nullptr /* configs */,
                                            bld_params,
                                            nullopt /* tenant */,
                                            false /* archived */));

        prep_bld_query bld_prep_query (
          build_db_->prepare_query<package_build_count> (
            "mod-builds-build-count-query", bq));

        size_t nt (tc == "*" ? toolchains.size () : 1);

        // The number of packages can potentially be large, and we may
        // implement some caching in the future. However, the caching will not
        // be easy as the cached values depend on the filter form parameters.
        //
        query<buildable_package> q (
          package_query<buildable_package> (
            params, tn, false /* archived */));

        for (auto& bp: build_db_->query<buildable_package> (q))
        {
          id = move (bp.id);

          shared_ptr<build_package> p (build_db_->load<build_package> (id));

          for (const auto& c: configs)
          {
            if (exclude (p->builds, p->constraints, *c))
            {
              nmax -= nt;

              config = c->name;
              ncur -= bld_prep_query.execute_value ();
            }
          }
        }
      }

      assert (nmax >= ncur);
      count = nmax - ncur;

      t.commit ();
    }

    // Print the filter form.
    //
    print_form (toolchains, count);

    // Print unbuilt package configurations with the following sort priority:
    //
    // 1: package name
    // 2: package version (descending)
    // 3: package tenant
    // 4: toolchain name
    // 5: toolchain version (descending)
    // 6: configuration name
    //
    // Prepare the build package prepared query.
    //
    // Note that we can't skip the proper number of packages in the database
    // query for a page numbers greater than one. So we will query packages
    // from the very beginning and skip the appropriate number of them while
    // iterating through the query result.
    //
    // Also note that such an approach has a security implication. An HTTP
    // request with a large page number will be quite expensive to process, as
    // it effectively results in traversing all the build package and all the
    // built configurations. To address this problem we may consider to reduce
    // the pager to just '<Prev' '1' 'Next>' links, and pass the offset as a
    // URL query parameter. Alternatively, we can invent the page number cap.
    //
    using pkg_query = query<buildable_package>;
    using prep_pkg_query = prepared_query<buildable_package>;

    pkg_query pq (
      package_query<buildable_package> (params, tn, false /* archived */));

    // Specify the portion. Note that we will still be querying packages in
    // chunks, not to hold locks for too long. For each package we will query
    // its builds, so let's keep the portion small.
    //
    size_t offset (0);

    pq += "ORDER BY" +
      pkg_query::build_package::id.name +
      order_by_version_desc (pkg_query::build_package::id.version,
                             false /* first */) + "," +
      pkg_query::build_package::id.tenant +
      "OFFSET" + pkg_query::_ref (offset) + "LIMIT 50";

    connection_ptr conn (build_db_->connection ());

    prep_pkg_query pkg_prep_query (
      conn->prepare_query<buildable_package> ("mod-builds-package-query", pq));

    // Prepare the build prepared query.
    //
    // For each package we will generate a set of all possible builds. Then,
    // iterating over the actual builds for the package we will exclude them
    // from the set of possible ones. The resulted set represents unbuilt
    // package configurations, and so will be printed.
    //
    using bld_query = query<package_build>;
    using prep_bld_query = prepared_query<package_build>;

    package_id id;

    bld_query bq (
      package_id_eq<package_build> (bld_query::build::id.package, id) &&

      // Note that the query already constrains the tenant via the build
      // package id.
      //
      build_query<package_build> (
        &conf_names, bld_params, nullopt /* tenant */, false /* archived */));

    prep_bld_query bld_prep_query (
      conn->prepare_query<package_build> ("mod-builds-build-query", bq));

    size_t skip (page * page_configs);
    size_t print (page_configs);

    // Enclose the subsequent tables to be able to use nth-child CSS selector.
    //
    s << DIV;
    while (print != 0)
    {
      transaction t (conn->begin ());

      // Query (and cache) buildable packages.
      //
      auto packages (pkg_prep_query.execute ());

      if (packages.empty ())
        print = 0;
      else
      {
        offset += packages.size ();

        // Iterate over packages and print unbuilt configurations. Skip the
        // appropriate number of them first (for page number greater than one).
        //
        for (auto& p: packages)
        {
          id = move (p.id);

          // Copy configuration/toolchain combinations for this package,
          // skipping excluded configurations.
          //
          set<config_toolchain> unbuilt_configs;
          {
            shared_ptr<build_package> p (build_db_->load<build_package> (id));

            for (const auto& ct: config_toolchains)
            {
              auto i (build_conf_map_->find (ct.configuration.c_str ()));
              assert (i != build_conf_map_->end ());

              if (!exclude (p->builds, p->constraints, *i->second))
                unbuilt_configs.insert (ct);
            }
          }

          // Iterate through the package configuration builds and erase them
          // from the unbuilt configurations set.
          //
          for (const auto& pb: bld_prep_query.execute ())
          {
            const build& b (*pb.build);

            unbuilt_configs.erase ({
                b.id.configuration, b.toolchain_name, b.toolchain_version});
          }

          // Print unbuilt package configurations.
          //
          for (const auto& ct: unbuilt_configs)
          {
            if (skip != 0)
            {
              --skip;
              continue;
            }

            auto i (build_conf_map_->find (ct.configuration.c_str ()));
            assert (i != build_conf_map_->end ());

            s << TABLE(CLASS="proplist build")
              <<   TBODY
              <<     TR_NAME (id.name, string (), root, id.tenant)
              <<     TR_VERSION (id.name, p.version, root, id.tenant)
              <<     TR_VALUE ("toolchain",
                               string (ct.toolchain_name) + '-' +
                               ct.toolchain_version.string ())
              <<     TR_VALUE ("config", ct.configuration)
              <<     TR_VALUE ("target", i->second->target.string ());

            // In the global view mode add the tenant builds link. Note that
            // the global view (and the link) makes sense only in the
            // multi-tenant mode.
            //
            if (!tn && !id.tenant.empty ())
              s << TR_TENANT (tenant_name, "builds", root, id.tenant);

            s <<   ~TBODY
              << ~TABLE;

            if (--print == 0) // Bail out the configuration loop.
              break;
          }

          if (print == 0) // Bail out the package loop.
            break;
        }
      }

      t.commit ();
    }
    s << ~DIV;
  }

  string u (tenant_dir (root, tenant).string () + "?builds");

  if (!params.name ().empty ())
  {
    u += '=';
    u += mime_url_encode (params.name ());
  }

  auto add_filter = [&u] (const char* pn,
                          const string& pv,
                          const char* def = "")
  {
    if (pv != def)
    {
      u += '&';
      u += pn;
      u += '=';
      u += mime_url_encode (pv);
    }
  };

  add_filter ("pv", params.version ());
  add_filter ("tc", params.toolchain (), "*");
  add_filter ("cf", params.configuration ());
  add_filter ("mn", params.machine ());
  add_filter ("tg", params.target ());
  add_filter ("rs", params.result (), "*");

  s <<       DIV_PAGER (page, count, page_configs, options_->build_pages (), u)
    <<     ~DIV
    <<   ~BODY
    << ~HTML;

  return true;
}
