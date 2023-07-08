// file      : mod/mod-builds.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-builds.hxx>

#include <set>

#include <libstudxml/serializer.hxx>

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <libbutl/utility.hxx>      // compare_c_string
#include <libbutl/timestamp.hxx>    // to_string()
#include <libbutl/path-pattern.hxx>

#include <libbbot/manifest.hxx> // to_result_status(), to_string(result_status)

#include <web/server/module.hxx>
#include <web/server/mime-url-encoding.hxx>

#include <web/xhtml/serialization.hxx>

#include <libbrep/build.hxx>
#include <libbrep/build-odb.hxx>
#include <libbrep/build-package.hxx>
#include <libbrep/build-package-odb.hxx>

#include <mod/page.hxx>
#include <mod/module-options.hxx>

using namespace std;
using namespace butl;
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

// Transform the wildcard to the SIMILAR TO-pattern.
//
static string
transform (const string& pattern)
{
  if (pattern.empty ())
    return "%";

  string r;
  for (const path_pattern_term& pt: path_pattern_iterator (pattern))
  {
    switch (pt.type)
    {
    case path_pattern_term_type::question: r += '_'; break;
    case path_pattern_term_type::star:     r += '%'; break;
    case path_pattern_term_type::bracket:
      {
        // Copy the bracket expression translating the inverse character, if
        // present.
        //
        size_t n (r.size ());
        r.append (pt.begin, pt.end);

        if (r[n + 1] == '!') // ...[!... ?
          r[n + 1] = '^';

        break;
      }
    case path_pattern_term_type::literal:
      {
        char c (get_literal (pt));

        // Escape the special characters.
        //
        // Note that '.' is not a special character for SIMILAR TO.
        //
        switch (c)
        {
        case '\\':
        case '%':
        case '_':
        case '|':
        case '+':
        case '{':
        case '}':
        case '(':
        case ')':
        case '[':
        case ']': r += '\\'; break;
        }

        r += c;
        break;
      }
    }
  }

  return r;
}

template <typename T, typename C>
static inline query<T>
match (const C qc, const string& pattern)
{
  return qc + "SIMILAR TO" + query<T>::_val (transform (pattern));
}

// If tenant is absent, then query builds from all the public tenants.
//
template <typename T>
static inline query<T>
build_query (const brep::vector<brep::build_target_config_id>* config_ids,
             const brep::params::builds& params,
             const brep::optional<brep::string>& tenant)
{
  using namespace brep;
  using query = query<T>;
  using qb = typename query::build;
  using qt = typename query::build_tenant;

  const auto& pid (qb::id.package);

  query q (tenant ? pid.tenant == *tenant : !qt::private_);

  if (config_ids != nullptr)
  {
    query sq (false);
    for (const auto& id: *config_ids)
      sq = sq || (qb::id.target == id.target &&
                  qb::id.target_config_name == id.config);

    q = q && sq;
  }

  // Note that there is no error reported if the filter parameters parsing
  // fails. Instead, it is considered that no package builds match such a
  // query.
  //
  try
  {
    // Package name.
    //
    if (!params.name ().empty ())
      q = q && match<T> (pid.name, params.name ());

    // Package version.
    //
    if (!params.version ().empty () && params.version () != "*")
    {
      // May throw invalid_argument.
      //
      version v (params.version (), version::none);

      q = q && compare_version_eq (pid.version,
                                   canonical_version (v),
                                   v.revision.has_value ());
    }

    // Build toolchain name/version.
    //
    const string& th (params.toolchain ());

    if (th != "*")
    {
      size_t p (th.find ('-'));
      if (p == string::npos) // Invalid format.
        throw invalid_argument ("");

      // Note that the toolchain version is selected from the list and denotes
      // the exact version revision, so an absent and zero revisions have the
      // same semantics and the zero revision is folded.
      //
      string  tn (th, 0, p);
      version tv (string (th, p + 1)); // May throw invalid_argument.

      q = q                           &&
          qb::id.toolchain_name == tn &&
          compare_version_eq (qb::id.toolchain_version,
                              canonical_version (tv),
                              true /* revision */);
    }

    // Build target.
    //
    if (!params.target ().empty ())
      q = q && match<T> (qb::id.target, params.target ());

    // Build target configuration name.
    //
    if (!params.target_config ().empty ())
      q = q && match<T> (qb::id.target_config_name, params.target_config ());

    // Build package configuration name.
    //
    if (!params.package_config ().empty ())
      q = q && match<T> (qb::id.package_config_name, params.package_config ());

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

        // May throw invalid_argument.
        //
        result_status st (bbot::to_result_status (rs));

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

// If tenant is absent, then query packages from all the public tenants.
//
template <typename T>
static inline query<T>
package_query (const brep::params::builds& params,
               const brep::optional<brep::string>& tenant)
{
  using namespace brep;
  using query = query<T>;
  using qp = typename query::build_package;
  using qt = typename query::build_tenant;

  query q (tenant ? qp::id.tenant == *tenant : !qt::private_);

  // Note that there is no error reported if the filter parameters parsing
  // fails. Instead, it is considered that no packages match such a query.
  //
  try
  {
    // Package name.
    //
    if (!params.name ().empty ())
      q = q && match<T> (qp::id.name, params.name ());

    // Package version.
    //
    if (!params.version ().empty () && params.version () != "*")
    {
      // May throw invalid_argument.
      //
      version v (params.version (), version::none);

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
  // builds from all the public tenants.
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
      string cth ("*");
      vector<pair<string, string>> toolchain_opts ({{"*", "*"}});
      {
        for (const auto& t: toolchains)
        {
          string th (t.first + '-' + t.second.string ());
          toolchain_opts.emplace_back (th, th);

          if (th == params.toolchain ())
            cth = move (th);
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
        <<       TR_SELECT ("toolchain", "th", cth, toolchain_opts)
        <<       TR_INPUT  ("target", "tg", params.target (), "*")

        <<       TR(CLASS="tgt-config")
        <<         TH << "tgt config" << ~TH
        <<         TD
        <<           *INPUT(TYPE="text",
                            NAME="tc",
                            VALUE=params.target_config (),
                            PLACEHOLDER="*",
                            LIST="target-configs")
        <<          DATALIST(ID="target-configs")
        <<            *OPTION(VALUE="*");

      // Print unique config names from the target config map.
      //
      set<const char*, butl::compare_c_string> conf_names;
      for (const auto& c: *target_conf_map_)
      {
        if (conf_names.insert (c.first.config.get ().c_str ()).second)
          s << *OPTION(VALUE=c.first.config.get ());
      }

      s <<          ~DATALIST
        <<         ~TD
        <<       ~TR

        <<       TR(CLASS="pkg-config")
        <<         TH << "pkg config" << ~TH
        <<         TD
        <<           *INPUT(TYPE="text",
                            NAME="pc",
                            VALUE=params.package_config (),
                            PLACEHOLDER="*")
        <<         ~TD
        <<       ~TR
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

  const string& tgt     (params.target ());
  const string& tgt_cfg (params.target_config ());
  const string& pkg_cfg (params.package_config ());

  // We will not display hidden configurations, unless the configuration is
  // specified explicitly.
  //
  bool exclude_hidden (tgt_cfg.empty () || path_pattern (tgt_cfg));

  vector<build_target_config_id> conf_ids;
  conf_ids.reserve (target_conf_map_->size ());

  for (const auto& c: *target_conf_map_)
  {
    if (!exclude_hidden || belongs (*c.second, "all"))
      conf_ids.push_back (c.first);
  }

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
    vector<package_build> builds;
    builds.reserve (page_configs);

    // Prepare the package build prepared query.
    //
    using query = query<package_build>;
    using prep_query = prepared_query<package_build>;

    query q (build_query<package_build> (&conf_ids, params, tn));

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

          auto i (target_conf_map_->find (
                    build_target_config_id {b->target,
                                            b->target_config_name}));

          assert (i != target_conf_map_->end ());

          // Match the target configuration against the package build
          // configuration expressions/constraints.
          //
          shared_ptr<build_package> p (
            build_db_->load<build_package> (b->id.package));

          const build_package_config* pc (find (b->package_config_name,
                                                p->configs));

          // The package configuration should be present since the
          // configurations set cannot change if the package version doesn't
          // change. If that's not the case, then the database has probably
          // been manually amended. In this case let's just skip such a build
          // as if it excluded and log the warning.
          //
          if (pc == nullptr)
          {
            warn << "cannot find configuration '" << b->package_config_name
                 << "' for package " << p->id.name << '/' << p->version;

            continue;
          }

          build_db_->load (*p, p->constraints_section);

          if (!exclude (*pc, p->builds, p->constraints, *i->second))
          {
            if (skip != 0)
              --skip;
            else if (print != 0)
            {
              // As we query builds in multiple transactions we may see the
              // same build multiple times. Let's skip the duplicates. Note:
              // we don't increment the counter in this case.
              //
              if (find_if (builds.begin (), builds.end (),
                           [&b] (const package_build& pb)
                           {
                             return b->id == pb.build->id;
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

              builds.push_back (move (pb));

              --print;
            }

            ++count;
          }
        }
      }
      //
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
    for (const package_build& pb: builds)
    {
      const build& b (*pb.build);

      string ts (butl::to_string (b.timestamp,
                                  "%Y-%m-%d %H:%M:%S %Z",
                                  true /* special */,
                                  true /* local */) +
                 " (" + butl::to_string (now - b.timestamp, false) + " ago");

      if (pb.archived)
        ts += ", archived";

      ts += ')';

      s << TABLE(CLASS="proplist build")
        <<   TBODY
        <<     TR_NAME (b.package_name, string (), root, b.tenant)
        <<     TR_VERSION (b.package_name, b.package_version, root, b.tenant)
        <<     TR_VALUE ("toolchain",
                         b.toolchain_name + '-' +
                         b.toolchain_version.string ())
        <<     TR_VALUE ("target", b.target.string ())
        <<     TR_VALUE ("tgt config", b.target_config_name)
        <<     TR_VALUE ("pkg config", b.package_config_name)
        <<     TR_VALUE ("timestamp", ts);

      if (b.interactive) // Note: can only be present for the building state.
        s <<   TR_VALUE ("login", *b.interactive);

      s <<     TR_BUILD_RESULT (b, pb.archived, host, root);

      // In the global view mode add the tenant builds link. Note that the
      // global view (and the link) makes sense only in the multi-tenant mode.
      //
      if (!tn && !b.tenant.empty ())
        s <<   TR_TENANT (tenant_name, "builds", root, b.tenant);

      s <<   ~TBODY
        << ~TABLE;
    }
    s << ~DIV;
  }
  else // Print unbuilt package configurations.
  {
    // Parameters to use for package build configurations queries. Note that
    // we cleanup the result filter argument, as it is irrelevant for unbuilt
    // configurations.
    //
    params::builds bld_params (params);
    bld_params.result () = "*";

    // Query toolchains, filter build target configurations and toolchains,
    // and create the set of target configuration/toolchain combinations, that
    // we will print for package configurations. Also calculate the number of
    // unbuilt package configurations.
    //
    toolchains toolchains;

    // Target configuration/toolchain combination.
    //
    // Note: all members are the shallow references.
    //
    struct target_config_toolchain
    {
      const butl::target_triplet& target;
      const string& target_config;
      const string& toolchain_name;
      const bpkg::version& toolchain_version;
    };

    vector<target_config_toolchain> config_toolchains;
    {
      transaction t (build_db_->begin ());
      toolchains = query_toolchains ();

      string th_name;
      version th_version;
      const string& th (params.toolchain ());

      if (th != "*")
      try
      {
        size_t p (th.find ('-'));
        if (p == string::npos)         // Invalid format.
          throw invalid_argument ("");

        th_name.assign (th, 0, p);

        // May throw invalid_argument.
        //
        // Note that an absent and zero revisions have the same semantics,
        // so the zero revision is folded (see above for details).
        //
        th_version = version (string (th, p + 1));
      }
      catch (const invalid_argument&)
      {
        // This is unlikely to be the user fault, as he selects the toolchain
        // from the list.
        //
        throw invalid_request (400, "invalid toolchain");
      }

      vector<const build_target_config*> target_configs;

      for (const auto& c: *target_conf_)
      {
            // Filter by name.
            //
        if ((tgt_cfg.empty () || path_match (c.name, tgt_cfg)) &&

            // Filter by target.
            //
            (tgt.empty () || path_match (c.target.string (), tgt)) &&

            (!exclude_hidden || belongs (c, "all"))) // Filter hidden.
        {
          target_configs.push_back (&c);

          for (const auto& t: toolchains)
          {
            // Filter by toolchain.
            //
            if (th == "*" || (t.first == th_name && t.second == th_version))
              config_toolchains.push_back (
                target_config_toolchain {c.target, c.name, t.first, t.second});
          }
        }
      }

      if (!config_toolchains.empty ())
      {
        // Calculate the number of unbuilt package configurations as a
        // difference between the possible number of unbuilt configurations
        // and the number of existing package builds.
        //
        // Note that some existing builds can now be excluded by package
        // configurations due to the build target configuration class set
        // change. We should deduct such builds count from the number of
        // existing package configurations builds.
        //
        // The only way to calculate both numbers is to traverse through the
        // package configurations and match their build
        // expressions/constraints against our target configurations.
        //
        size_t npos (0);

        size_t ncur (build_db_->query_value<package_build_count> (
          build_query<package_build_count> (&conf_ids, bld_params, tn)));

        // From now we will be using specific values for the below filters for
        // each build database query. Note that the toolchain is the only
        // filter left in bld_params.
        //
        bld_params.name ().clear ();
        bld_params.version ().clear ();
        bld_params.target ().clear ();
        bld_params.target_config ().clear ();
        bld_params.package_config ().clear ();

        // Prepare the build count prepared query.
        //
        // For each package-excluded configuration we will query the number of
        // existing builds.
        //
        using bld_query = query<package_build_count>;
        using prep_bld_query = prepared_query<package_build_count>;

        package_id id;
        target_triplet target;
        string target_config_name;
        string package_config_name;

        const auto& bid (bld_query::build::id);

        bld_query bq (
          equal<package_build_count> (bid.package, id)                     &&
          bid.target == bld_query::_ref (target)                           &&
          bid.target_config_name == bld_query::_ref (target_config_name)   &&
          bid.package_config_name == bld_query::_ref (package_config_name) &&

          // Note that the query already constrains configurations via the
          // configuration name and target.
          //
          // Also note that while the query already constrains the tenant via
          // the build package id, we still need to pass the tenant not to
          // erroneously filter out the private tenants.
          //
          build_query<package_build_count> (nullptr /* config_ids */,
                                            bld_params,
                                            tn));

        prep_bld_query bld_prep_query (
          build_db_->prepare_query<package_build_count> (
            "mod-builds-build-count-query", bq));

        // Number of possible builds per package configuration.
        //
        size_t nt (th == "*" ? toolchains.size () : 1);

        // The number of packages can potentially be large, and we may
        // implement some caching in the future. However, the caching will not
        // be easy as the cached values depend on the filter form parameters.
        //
        query<buildable_package> q (
          package_query<buildable_package> (params, tn));

        for (auto& bp: build_db_->query<buildable_package> (q))
        {
          shared_ptr<build_package>& p (bp.package);

          id = p->id;

          // Note: load the constrains section lazily.
          //
          for (const build_package_config& c: p->configs)
          {
            // Filter by package config name.
            //
            if (pkg_cfg.empty () || path_match (c.name, pkg_cfg))
            {
              for (const auto& tc: target_configs)
              {
                if (!p->constraints_section.loaded ())
                  build_db_->load (*p, p->constraints_section);

                if (exclude (c, p->builds, p->constraints, *tc))
                {
                  target = tc->target;
                  target_config_name = tc->name;
                  package_config_name = c.name;
                  ncur -= bld_prep_query.execute_value ();
                }
                else
                  npos += nt;
              }
            }
          }
        }

        assert (npos >= ncur);
        count = npos - ncur;
      }
      else
        count = 0;

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
    // 6: target
    // 7: target configuration name
    // 8: package configuration name
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

    pkg_query pq (package_query<buildable_package> (params, tn));

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

    bld_query bq (equal<package_build> (bld_query::build::id.package, id) &&

                  // Note that while the query already constrains the tenant
                  // via the build package id, we still need to pass the
                  // tenant not to erroneously filter out the private tenants.
                  //
                  build_query<package_build> (&conf_ids, bld_params, tn));

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
        for (auto& bp: packages)
        {
          shared_ptr<build_package>& p (bp.package);

          id = p->id;

          // Copy configuration/toolchain combinations for this package,
          // skipping excluded configurations.
          //
          set<config_toolchain> unbuilt_configs;

          // Load the constrains section lazily.
          //
          for (const build_package_config& pc: p->configs)
          {
            // Filter by package config name.
            //
            if (pkg_cfg.empty () || path_match (pc.name, pkg_cfg))
            {
              for (const target_config_toolchain& ct: config_toolchains)
              {
                auto i (
                  target_conf_map_->find (
                    build_target_config_id {ct.target, ct.target_config}));

                assert (i != target_conf_map_->end ());

                if (!p->constraints_section.loaded ())
                  build_db_->load (*p, p->constraints_section);

                if (!exclude (pc, p->builds, p->constraints, *i->second))
                  unbuilt_configs.insert (
                    config_toolchain {ct.target,
                                      ct.target_config,
                                      pc.name,
                                      ct.toolchain_name,
                                      ct.toolchain_version});
              }
            }
          }

          // Iterate through the package configuration builds and erase them
          // from the unbuilt configurations set.
          //
          for (const auto& pb: bld_prep_query.execute ())
          {
            const build& b (*pb.build);

            unbuilt_configs.erase (config_toolchain {b.target,
                                                     b.target_config_name,
                                                     b.package_config_name,
                                                     b.toolchain_name,
                                                     b.toolchain_version});
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

            s << TABLE(CLASS="proplist build")
              <<   TBODY
              <<     TR_NAME (id.name, string (), root, id.tenant)
              <<     TR_VERSION (id.name, p->version, root, id.tenant)
              <<     TR_VALUE ("toolchain",
                               string (ct.toolchain_name) + '-' +
                               ct.toolchain_version.string ())
              <<     TR_VALUE ("target", ct.target.string ())
              <<     TR_VALUE ("tgt config", ct.target_config)
              <<     TR_VALUE ("pkg config", ct.package_config);

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
  add_filter ("th", params.toolchain (), "*");
  add_filter ("tg", tgt);
  add_filter ("tc", tgt_cfg);
  add_filter ("pc", pkg_cfg);
  add_filter ("rs", params.result (), "*");

  s <<       DIV_PAGER (page, count, page_configs, options_->build_pages (), u)
    <<     ~DIV
    <<   ~BODY
    << ~HTML;

  return true;
}
