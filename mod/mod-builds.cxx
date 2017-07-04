// file      : mod/mod-builds.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-builds.hxx>

#include <set>

#include <libstudxml/serializer.hxx>

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <libbutl/timestamp.hxx>  // to_string()
#include <libbutl/filesystem.hxx> // path_match()

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
#include <mod/build-config.hxx> // *_url()

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
      options_ (r.initialized_ ? r.options_ : nullptr)
{
}

void brep::builds::
init (scanner& s)
{
  MODULE_DIAG;

  options_ = make_shared<options::builds> (
    s, unknown_mode::fail, unknown_mode::fail);

  if (options_->build_config_specified ())
    database_module::init (static_cast<options::build>    (*options_),
                           static_cast<options::build_db> (*options_),
                           options_->build_db_retry ());

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
build_query (const brep::cstrings& configs, const brep::params::builds& params)
{
  using namespace brep;
  using query = query<T>;
  using qb = typename query::build;

  query q (qb::id.configuration.in_range (configs.begin (), configs.end ()));

  // Note that there is no error reported if the filter parameters parsing
  // fails. Instead, it is considered that no package builds match such a
  // query.
  //
  try
  {
    // Package name.
    //
    if (!params.name ().empty ())
      q = q && qb::id.package.name.like (transform (params.name ()));

    // Package version.
    //
    if (!params.version ().empty () && params.version () != "*")
      q = q && compare_version_eq (qb::id.package.version,
                                   version (params.version ()), // May throw.
                                   true);

    // Build toolchain name/version.
    //
    const string& tc (params.toolchain ());

    if (tc != "*")
    {
      size_t p (tc.find ('-'));
      if (p == string::npos) // Invalid format.
        throw invalid_argument ("");

      string  tn (tc, 0, p);
      version tv (string (tc, p + 1)); // May throw invalid_argument.

      q = q && qb::toolchain_name == tn &&
        compare_version_eq (qb::id.toolchain_version, tv, true);
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
    const string& tg (params.target ());

    if (tg != "*")
      q = q && (tg.empty ()
                ? qb::target.is_null ()
                : qb::target.like (transform (tg)));

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
package_query (const brep::params::builds& params)
{
  using namespace brep;
  using query = query<T>;
  using qp = typename query::build_package;

  query q (true);

  // Note that there is no error reported if the filter parameters parsing
  // fails. Instead, it is considered that no packages match such a query.
  //
  try
  {
    // Package name.
    //
    if (!params.name ().empty ())
      q = q && qp::id.name.like (transform (params.name ()));

    // Package version.
    //
    if (!params.version ().empty () && params.version () != "*")
      q = q && compare_version_eq (qp::id.version,
                                   version (params.version ()), // May throw.
                                   true);
  }
  catch (const invalid_argument&)
  {
    return query (false);
  }

  return q;
}

static const vector<pair<string, string>> build_results ({
    {"unbuilt", "<unbuilt>"},
    {"*", "*"},
    {"pending", "pending"},
    {"building", "building"},
    {"success", "success"},
    {"warning", "warning"},
    {"error", "error"},
    {"abort", "abort"},
    {"abnormal", "abnormal"}});

bool brep::builds::
handle (request& rq, response& rs)
{
  using brep::version;
  using namespace web::xhtml;

  MODULE_DIAG;

  if (build_db_ == nullptr)
    throw invalid_request (501, "not implemented");

  const size_t page_configs (options_->build_configurations ());
  const string& host (options_->host ());
  const dir_path& root (options_->root ());

  params::builds params;

  try
  {
    name_value_scanner s (rq.parameters ());
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
    // element of the search form. The problem appears in Firefox and has a
    // (4-year old, at the time of this writing) bug report:
    //
    // https://bugzilla.mozilla.org/show_bug.cgi?id=712130.
    //
    <<     SCRIPT << " " << ~SCRIPT
    <<   ~HEAD
    <<   BODY
    <<     DIV_HEADER (root, options_->logo (), options_->menu ())
    <<     DIV(ID="content");

  // Return the list of distinct toolchain name/version pairs. The build db
  // transaction must be started.
  //
  using toolchains = vector<pair<string, version>>;

  auto query_toolchains = [this] () -> toolchains
  {
    using query = query<toolchain>;

    toolchains r;
    for (auto& t: build_db_->query<toolchain> (
           "ORDER BY" + query::toolchain_name +
           order_by_version_desc (query::id.toolchain_version, false)))
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
      // doesn't specify browser behavior explicitly for the case the attribute
      // is omitted, the only reasonable behavior is to default it to the
      // current document URL. Note that we specify the function name using the
      // "hidden" <input/> element since the action url must not contain the
      // query part.
      //
      s << FORM
        <<   *INPUT(TYPE="hidden", NAME="builds")
        <<   TABLE(ID="filter", CLASS="proplist")
        <<     TBODY
        <<       TR_INPUT  ("name", "pn", params.name (), "*", true)
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
        <<       TR_INPUT  ("target", "tg", params.target (), "<default>")
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

  size_t count;
  size_t page (params.page ());

  if (params.result () != "unbuilt")
  {
    transaction t (build_db_->begin ());

    count = build_db_->query_value<package_build_count> (
      build_query<package_build_count> (*build_conf_names_, params));

    // Print the filter form.
    //
    print_form (query_toolchains (), count);

    // Print package build configurations ordered by the timestamp (later goes
    // first).
    //
    timestamp now (timestamp::clock::now ());

    // Enclose the subsequent tables to be able to use nth-child CSS selector.
    //
    s << DIV;
    for (auto& pb: build_db_->query<package_build> (
           build_query<package_build> (*build_conf_names_, params) +
           "ORDER BY" + query<build>::timestamp + "DESC" +
           "OFFSET" + to_string (page * page_configs) +
           "LIMIT" + to_string (page_configs)))
    {
      build& b (*pb.build);

      string ts (butl::to_string (b.timestamp,
                                  "%Y-%m-%d %H:%M:%S %Z",
                                  true,
                                  true) +
                 " (" + butl::to_string (now - b.timestamp, false) + " ago)");

      if (b.state == build_state::built)
        build_db_->load (b, b.results_section);

      s << TABLE(CLASS="proplist build")
        <<   TBODY
        <<     TR_NAME (b.package_name, string (), root)
        <<     TR_VERSION (b.package_name, b.package_version, root)
        <<     TR_VALUE ("toolchain",
                         b.toolchain_name + '-' +
                         b.toolchain_version.string ())
        <<     TR_VALUE ("config", b.configuration)
        <<     TR_VALUE ("machine", b.machine)
        <<     TR_VALUE ("target",
                         b.target ? b.target->string () : "<default>")
        <<     TR_VALUE ("timestamp", ts)
        <<     TR_BUILD_RESULT (b, host, root)
        <<   ~TBODY
        << ~TABLE;
    }
    s << ~DIV;

    t.commit ();
  }
  else // Print unbuilt package configurations.
  {
    // Parameters to use for package build configurations queries. Note that we
    // cleanup the machine and the result filter arguments, as they are
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

    struct config_toolchain
    {
      const string& configuration;
      const string& toolchain_name;
      const version& toolchain_version;

      bool
      operator< (const config_toolchain& ct) const
      {
        int r (configuration.compare (ct.configuration));
        if (r != 0)
          return r < 0;

        r = toolchain_name.compare (ct.toolchain_name);
        if (r != 0)
          return r < 0;

        return toolchain_version > ct.toolchain_version;
      }
    };

    // Note that config_toolchains contains shallow references to the toolchain
    // names and versions, and in particular to the selected ones (tc_name and
    // tc_version).
    //
    string tc_name;
    version tc_version;
    set<config_toolchain> config_toolchains;

    {
      transaction t (build_db_->begin ());
      toolchains = query_toolchains ();

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

      for (const auto& c: *build_conf_)
      {
        if ((pc.empty () || path_match (pc, c.name)) && // Filter by name.

            (tg.empty ()                                // Filter by target.
             ? !c.target
             : tg == "*" ||
               (c.target && path_match (tg, c.target->string ()))))
        {
          if (tc != "*") // Filter by toolchain.
            config_toolchains.insert ({c.name, tc_name, tc_version});
          else           // Add all toolchains.
          {
            for (const auto& t: toolchains)
              config_toolchains.insert ({c.name, t.first, t.second});
          }
        }
      }

      // Calculate the number of unbuilt package configurations as a
      // difference between the maximum possible number of unbuilt
      // configurations and the number of existing package builds.
      //
      size_t nmax (config_toolchains.size () *
                   build_db_->query_value<buildable_package_count> (
                     package_query<buildable_package_count> (params)));

      size_t ncur = build_db_->query_value<package_build_count> (
        build_query<package_build_count> (*build_conf_names_, bld_params));

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
    // 3: configuration name
    // 4: toolchain name
    // 5: toolchain version (descending)
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

    pkg_query pq (package_query<buildable_package> (params));

    // Specify the portion. Note that we will still be querying packages in
    // chunks, not to hold locks for too long.
    //
    size_t offset (0);

    pq += "ORDER BY" +
      pkg_query::build_package::id.name +
      order_by_version_desc (pkg_query::build_package::id.version, false) +
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

    // We will use specific package name and version for each database query.
    //
    bld_params.name ().clear ();
    bld_params.version ().clear ();

    package_id id;
    const auto& qv (bld_query::build::id.package.version);

    bld_query bq (
      bld_query::build::id.package.name == bld_query::_ref (id.name) &&

      qv.epoch == bld_query::_ref (id.version.epoch) &&
      qv.canonical_upstream ==
      bld_query::_ref (id.version.canonical_upstream) &&
      qv.canonical_release ==
      bld_query::_ref (id.version.canonical_release) &&
      qv.revision == bld_query::_ref (id.version.revision) &&

      build_query<package_build> (*build_conf_names_, bld_params));

    prep_bld_query bld_prep_query (
      conn->prepare_query<package_build> ("mod-builds-build-query", bq));

    size_t skip (page * page_configs);
    size_t print (page_configs);

    // Enclose the subsequent tables to be able to use nth-child CSS selector.
    //
    s << DIV;
    for (bool prn (true); prn; )
    {
      transaction t (conn->begin ());

      // Query (and cache) buildable packages.
      //
      auto packages (pkg_prep_query.execute ());

      if ((prn = !packages.empty ()))
      {
        offset += packages.size ();

        // Iterate over packages and print unbuilt configurations. Skip the
        // appropriate number of them first (for page number greater than one).
        //
        for (auto& pv: packages)
        {
          id = move (pv.id);

          // Make a copy for this package.
          //
          auto unbuilt_configs (config_toolchains);

          // Iterate through the package configuration builds and erase them
          // from the unbuilt configurations set.
          //
          for (const auto& pb: bld_prep_query.execute ())
          {
            build& b (*pb.build);

            unbuilt_configs.erase ({
                b.id.configuration, b.toolchain_name, b.toolchain_version});
          }

          // Print unbuilt package configurations.
          //
          for (const auto& ct: unbuilt_configs)
          {
            if (skip > 0)
            {
              --skip;
              continue;
            }

            if (print-- == 0)
            {
              prn = false;
              break;
            }

            auto i (build_conf_map_->find (ct.configuration.c_str ()));
            assert (i != build_conf_map_->end ());

            const optional<target_triplet>& tg (i->second->target);

            s << TABLE(CLASS="proplist build")
              <<   TBODY
              <<     TR_NAME (id.name, string (), root)
              <<     TR_VERSION (id.name, pv.version, root)
              <<     TR_VALUE ("toolchain",
                               string (ct.toolchain_name) + '-' +
                               ct.toolchain_version.string ())
              <<     TR_VALUE ("config", ct.configuration)
              <<     TR_VALUE ("target", tg ? tg->string () : "<default>")
              <<   ~TBODY
              << ~TABLE;
          }

          if (!prn)
            break;
        }
      }

      t.commit ();
    }
    s << ~DIV;
  }

  string u (root.string () + "?builds");

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

  add_filter ("pn", params.name ());
  add_filter ("pv", params.version ());
  add_filter ("tc", params.toolchain (), "*");
  add_filter ("cf", params.configuration ());
  add_filter ("mn", params.machine ());
  add_filter ("tg", params.target (), "*");
  add_filter ("rs", params.result (), "*");

  s <<       DIV_PAGER (page, count, page_configs, options_->build_pages (), u)
    <<     ~DIV
    <<   ~BODY
    << ~HTML;

  return true;
}
