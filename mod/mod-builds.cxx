// file      : mod/mod-builds.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-builds.hxx>

#include <libstudxml/serializer.hxx>

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <libbutl/timestamp.hxx> // to_string()

#include <libbbot/manifest.hxx> // to_result_status(), to_string(result_status)

#include <web/xhtml.hxx>
#include <web/module.hxx>
#include <web/mime-url-encoding.hxx>

#include <libbrep/build.hxx>
#include <libbrep/build-odb.hxx>
#include <libbrep/package.hxx>
#include <libbrep/package-odb.hxx>

#include <mod/page.hxx>
#include <mod/options.hxx>
#include <mod/build-config.hxx> // *_url()

using namespace std;
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

  database_module::init (*options_, options_->package_db_retry ());

  if (options_->build_config_specified ())
    database_module::init (static_cast<options::build>    (*options_),
                           static_cast<options::build_db> (*options_),
                           options_->build_db_retry ());

  if (options_->root ().empty ())
    options_->root (dir_path ("/"));
}

template <typename T, typename C>
static inline query<T>
build_query (const C& configs, const brep::params::builds& params)
{
  using namespace brep;

  using query = query<T>;

  // Transform the wildcard to the LIKE-pattern.
  //
  auto transform = [] (const string& s) -> string
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
  };

  query q (
    query::id.configuration.in_range (configs.begin (), configs.end ()) &&
    (query::state == "testing" || query::state == "tested"));

  // Note that there is no error reported if the filter parameters parsing
  // fails. Instead, it is considered that no package builds match such a
  // query.
  //
  try
  {
    // Package name.
    //
    if (!params.name ().empty ())
      q = q && query::id.package.name.like (transform (params.name ()));

    // Package version.
    //
    if (!params.version ().empty () && params.version () != "*")
    {
      version v (params.version ()); // May throw invalid_argument.
      q = q && compare_version_eq (query::id.package.version, v, true);
    }

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

      q = q && query::toolchain_name == tn &&
        compare_version_eq (query::id.toolchain_version, tv, true);
    }

    // Build configuration name.
    //
    if (!params.configuration ().empty ())
      q = q && query::id.configuration.like (
        transform (params.configuration ()));

    // Build machine name.
    //
    if (!params.machine ().empty ())
      query::machine.like (transform (params.machine ()));

    // Build target.
    //
    const string& tg (params.target ());

    if (tg != "*")
      q = q && (tg.empty ()
                ? query::target.is_null ()
                : query::target.like (transform (tg)));

    // Build result.
    //
    const string& rs (params.result ());

    if (!rs.empty () && rs != "*")
    {
      if (rs == "pending")
        q = q && query::forced;
      else if (rs == "building")
        q = q && query::state == "testing";
      else
      {
        query sq (query::status == rs);
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
            sq = sq || query::status == to_string (st);
        }

        q = q && sq;
      }
    }
  }
  catch (const invalid_argument&)
  {
    return query (false);
  }

  return q;
}

bool brep::builds::
handle (request& rq, response& rs)
{
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
    params = params::builds (
      s, unknown_mode::fail, unknown_mode::fail);
  }
  catch (const cli::exception& e)
  {
    throw invalid_request (400, e.what ());
  }

  size_t page (params.page ());
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

  transaction t (build_db_->begin ());

  // Having packages and packages configurations builds in different databases,
  // we unable to filter out builds for non-existent packages at the query
  // level. Doing that in the C++ code would complicate it significantly. So
  // we will print all the builds, relying on the sorting algorithm, that will
  // likely to place expired ones at the end of the query result.
  //
  auto count (
    build_db_->query_value<build_count> (
      build_query<build_count> (*build_conf_names_, params)));

  // Print the package builds filter form on the first page only.
  //
  if (page == 0)
  {
    // Populate the toolchains list with the distinct list of toolchain
    // name/version pairs from all the existing package builds. Make sure the
    // selected toolchain still present in the database. Otherwise fallback to
    // the * wildcard selection.
    //
    string tc ("*");
    vector<pair<string,string>> toolchains ({{"*", "*"}});
    {
      using query = query<toolchain>;

      for (const auto& t: build_db_->query<toolchain> (
             "ORDER BY" + query::toolchain_name +
             order_by_version_desc (query::id.toolchain_version, false)))
      {
        string s (t.name + '-' + t.version.string ());
        toolchains.emplace_back (s, s);

        if (s == params.toolchain ())
          tc = move (s);
      }
    }

    // The 'action' attribute is optional in HTML5. While the standard doesn't
    // specify browser behavior explicitly for the case the attribute is
    // omitted, the only reasonable behavior is to default it to the current
    // document URL. Note that we specify the function name using the "hidden"
    // <input/> element since the action url must not contain the query part.
    //
    s << FORM
      <<   *INPUT(TYPE="hidden", NAME="builds")
      <<   TABLE(ID="filter", CLASS="proplist")
      <<     TBODY
      <<       TR_INPUT  ("name", "pn", params.name (), "*", true)
      <<       TR_INPUT  ("version", "pv", params.version (), "*")
      <<       TR_SELECT ("toolchain", "tc", tc, toolchains)

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

      <<       TR_INPUT ("machine", "mn", params.machine (), "*")
      <<       TR_INPUT ("target", "tg", params.target (), "<default>")
      <<       TR_INPUT ("result", "rs", params.result (), "*")
      <<     ~TBODY
      <<   ~TABLE
      <<   TABLE(CLASS="form-table")
      <<     TBODY
      <<       TR
      <<         TD(ID="build-count")
      <<           DIV_COUNTER (count, "Build", "Builds")
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
    s << DIV_COUNTER (count, "Build", "Builds");

  // Enclose the subsequent tables to be able to use nth-child CSS selector.
  //
  s << DIV;
  for (auto& b: build_db_->query<build> (
         build_query<build> (*build_conf_names_, params) +
         "ORDER BY" + query<build>::timestamp + "DESC" +
         "OFFSET" + to_string (page * page_configs) +
         "LIMIT" + to_string (page_configs)))
  {
    assert (b.machine);

    string ts (butl::to_string (b.timestamp,
                                "%Y-%m-%d %H:%M:%S%[.N] %Z",
                                true,
                                true));

    s << TABLE(CLASS="proplist build")
      <<   TBODY
      <<     TR_NAME (b.package_name, string (), root)
      <<     TR_VERSION (b.package_name, b.package_version, root)
      <<     TR_VALUE ("toolchain",
                       b.toolchain_name + '-' +
                       b.toolchain_version.string ())
      <<     TR_VALUE ("config", b.configuration)
      <<     TR_VALUE ("machine", *b.machine)
      <<     TR_VALUE ("target", b.target ? b.target->string () : "<default>")
      <<     TR_VALUE ("timestamp", ts)
      <<     TR(CLASS="result")
      <<       TH << "result" << ~TH
      <<       TD
      <<         SPAN(CLASS="value");

    if (b.state == build_state::testing)
      s << "building";
    else
    {
      assert (b.state == build_state::tested);

      build_db_->load (b, b.results_section);

      // If no unsuccessful operations results available, then print the
      // overall build status. If there are any operations results available,
      // then also print unsuccessful operations statuses with the links to the
      // respective logs, followed with a link to the operations combined log.
      // Print the forced package rebuild link afterwards, unless the package
      // build is already pending.
      //
      if (b.results.empty () || *b.status == result_status::success)
      {
        assert (b.status);
        s << SPAN_BUILD_RESULT_STATUS (*b.status) << " | ";
      }

      if (!b.results.empty ())
      {
        for (const auto& r: b.results)
        {
          if (r.status != result_status::success)
            s << SPAN_BUILD_RESULT_STATUS (r.status) << " ("
              << A
              <<   HREF
              <<     build_log_url (host, root, b, &r.operation)
              <<   ~HREF
              <<   r.operation
              << ~A
              << ") | ";
        }

        s << A
          <<   HREF << build_log_url (host, root, b) << ~HREF
          <<   "log"
          << ~A
          << " | ";
      }

      if (b.forced)
        s << "pending";
      else
        s << A
          <<   HREF << force_rebuild_url (host, root, b) << ~HREF
          <<   "rebuild"
          << ~A;
    }

    s <<         ~SPAN
      <<       ~TD
      <<     ~TR
      <<   ~TBODY
      << ~TABLE;
  }
  s << ~DIV;

  t.commit ();

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
  add_filter ("rs", params.result ());

  s <<       DIV_PAGER (page, count, page_configs, options_->build_pages (), u)
    <<     ~DIV
    <<   ~BODY
    << ~HTML;

  return true;
}
