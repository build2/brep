// file      : mod/mod-builds.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-builds.hxx>

#include <libstudxml/serializer.hxx>

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <libbutl/timestamp.hxx> // to_string()

#include <web/xhtml.hxx>
#include <web/module.hxx>

#include <libbrep/build.hxx>
#include <libbrep/build-odb.hxx>
#include <libbrep/package.hxx>
#include <libbrep/package-odb.hxx>

#include <mod/page.hxx>
#include <mod/options.hxx>
#include <mod/build-config.hxx> // *_url()

using namespace std;
using namespace bbot;
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
build_query (const C& configs)
{
  using query = query<T>;

  return query::id.configuration.in_range (configs.begin (), configs.end ()) &&
    (query::state == "testing" || query::state == "tested");
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
      build_query<build_count> (*build_conf_names_)));

  s << DIV_COUNTER (count, "Build", "Builds");

  // Enclose the subsequent tables to be able to use nth-child CSS selector.
  //
  s << DIV;
  for (auto& b: build_db_->query<build> (
         build_query<build> (*build_conf_names_) +
         "ORDER BY" + query<build>::timestamp + "DESC" +
         "OFFSET" + to_string (page * page_configs) +
         "LIMIT" + to_string (page_configs)))
  {
    assert (b.machine);

    auto i (build_conf_map_->find (b.configuration.c_str ()));
    assert (i != build_conf_map_->end ());
    const build_config& c (*i->second);

    string ts (butl::to_string (b.timestamp,
                                "%Y-%m-%d %H:%M:%S%[.N] %Z",
                                true, true));

    s << TABLE(CLASS="proplist build")
      <<   TBODY
      <<     TR_NAME (b.package_name, string (), root)
      <<     TR_VERSION (b.package_name, b.package_version, root)
      <<     TR_VALUE ("toolchain",
                       b.toolchain_name + '-' +
                       b.toolchain_version.string ())
      <<     TR_VALUE ("config", b.configuration)
      <<     TR_VALUE ("machine", *b.machine)
      <<     TR_VALUE ("target", c.target ? c.target->string () : "<default>")
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

  s <<       DIV_PAGER (page, count, page_configs, options_->build_pages (),
                        root.string () + "?builds")
    <<     ~DIV
    <<   ~BODY
    << ~HTML;

  return true;
}
