// file      : mod/mod-advanced-search.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-advanced-search.hxx>

#include <libstudxml/serializer.hxx>

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <web/server/module.hxx>
#include <web/server/mime-url-encoding.hxx>

#include <web/xhtml/serialization.hxx>

#include <libbrep/package.hxx>
#include <libbrep/package-odb.hxx>

#include <mod/page.hxx>
#include <mod/utility.hxx>        // wildcard_to_similar_to_pattern()
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
brep::advanced_search::
advanced_search (const advanced_search& r)
    : database_module (r),
      options_ (r.initialized_ ? r.options_ : nullptr)
{
}

void brep::advanced_search::
init (scanner& s)
{
  HANDLER_DIAG;

  options_ = make_shared<options::advanced_search> (
    s, unknown_mode::fail, unknown_mode::fail);

  database_module::init (*options_, options_->package_db_retry ());

  if (options_->root ().empty ())
    options_->root (dir_path ("/"));
}

template <typename T, typename C>
static inline query<T>
match (const C qc, const string& pattern)
{
  return qc           +
         "SIMILAR TO" +
         query<T>::_val (brep::wildcard_to_similar_to_pattern (pattern));
}

template <typename T>
static inline query<T>
package_query (const brep::params::advanced_search& params)
{
  using namespace brep;
  using query = query<T>;

  query q (query::internal_repository.canonical_name.is_not_null ());

  // Note that there is no error reported if the filter parameters parsing
  // fails. Instead, it is considered that no package builds match such a
  // query.
  //
  try
  {
    // Package name.
    //
    if (!params.name ().empty ())
      q = q && match<T> (query::id.name, params.name ());

    // Package version.
    //
    if (!params.version ().empty () && params.version () != "*")
    {
      // May throw invalid_argument.
      //
      version v (params.version (), version::none);

      q = q && compare_version_eq (query::id.version,
                                   canonical_version (v),
                                   v.revision.has_value ());
    }

    // Package project.
    //
    if (!params.project ().empty ())
      q = q && match<T> (query::project, params.project ());

    // Package repository.
    //
    const string& rp (params.repository ());

    if (rp != "*")
      q = q && query::internal_repository.canonical_name == rp;

    // Reviews.
    //
    const string& rs (params.reviews ());

    if (rs != "*")
    {
      if (rs == "reviewed")
        q = q && query::reviews.pass.is_not_null ();
      else if (rs == "unreviewed")
        q = q && query::reviews.pass.is_null ();
      else
        throw invalid_argument ("");
    }
  }
  catch (const invalid_argument&)
  {
    return query (false);
  }

  return q;
}

static const vector<pair<string, string>> reviews ({
    {"*",          "*"},
    {"reviewed",   "reviewed"},
    {"unreviewed", "unreviewed"}});

bool brep::advanced_search::
handle (request& rq, response& rs)
{
  using namespace web::xhtml;

  HANDLER_DIAG;

  // Note that while we could potentially support the multi-tenant mode, that
  // would require to invent the package/tenant view to filter out the private
  // tenants from the search. This doesn't look of much use at the moment.
  // Thus, let's keep it simple for now and just respond with the 501 status
  // code (not implemented) if such a mode is detected.
  //
  // NOTE: don't forget to update TR_PROJECT::operator() when/if this mode is
  //       supported.
  //
  if (!tenant.empty ())
    throw invalid_request (501, "not implemented");

  const size_t res_page (options_->search_page_entries ());
  const dir_path& root (options_->root ());

  params::advanced_search params;

  try
  {
    name_value_scanner s (rq.parameters (8 * 1024));
    params = params::advanced_search (s,
                                      unknown_mode::fail,
                                      unknown_mode::fail);
  }
  catch (const cli::exception& e)
  {
    throw invalid_request (400, e.what ());
  }

  const char* title ("Advanced Package Search");

  xml::serializer s (rs.content (), title);

  s << HTML
    <<   HEAD
    <<     TITLE << title << ~TITLE
    <<     CSS_LINKS (path ("advanced-search.css"), root)
    <<   ~HEAD
    <<   BODY
    <<     DIV_HEADER (options_->logo (), options_->menu (), root, tenant)
    <<     DIV(ID="content");

  transaction t (package_db_->begin ());

  size_t count (
    package_db_->query_value<package_count> (
      package_query<package_count> (params)));

  // Load the internal repositories as the canonical name/location pairs,
  // sorting them in the same way as on the About page.
  //
  vector<pair<string, string>> repos ({{"*", "*"}});
  {
    using query = query<repository>;

    for (repository& r:
           package_db_->query<repository> (
             (query::internal && query::id.tenant == tenant) +
             "ORDER BY" + query::priority))
    {
      repos.emplace_back (move (r.id.canonical_name), r.location.string ());
    }
  }

  // Print the package builds filter form on the first page only.
  //
  size_t page (params.page ());

  if (page == 0)
  {
    // The 'action' attribute is optional in HTML5. While the standard
    // doesn't specify browser behavior explicitly for the case the
    // attribute is omitted, the only reasonable behavior is to default it
    // to the current document URL.
    //
    s << FORM
      <<   TABLE(ID="filter", CLASS="proplist")
      <<     TBODY
      <<       TR_INPUT  ("name", "advanced-search", params.name (), "*", true)
      <<       TR_INPUT  ("version", "pv", params.version (), "*")
      <<       TR_INPUT  ("project", "pr", params.project (), "*")
      <<       TR_SELECT ("repository", "rp", params.repository (), repos);

    if (options_->reviews_url_specified ())
      s <<     TR_SELECT ("reviews", "rv", params.reviews (), reviews);

    s <<     ~TBODY
      <<   ~TABLE
      <<   TABLE(CLASS="form-table")
      <<     TBODY
      <<       TR
      <<         TD(ID="package-version-count")
      <<           DIV_COUNTER (count, "Package Version", "Package Versions")
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
    s << DIV_COUNTER (count, "Package Version", "Package Versions");

  using query = query<package>;

  // Note that we query an additional package version which we will not
  // display, but will use to check if it belongs to the same package and/or
  // project as the last displayed package version. If that's the case we will
  // display the '...' mark(s) at the end of the page, indicating that there a
  // more package versions from this package/project on the next page(s).
  //
  query q (package_query<package> (params)        +
           "ORDER BY tenant, project, name, version_epoch DESC, "
           "version_canonical_upstream DESC, version_canonical_release DESC, "
           "version_revision DESC"                +
           "OFFSET" + to_string (page * res_page) +
           "LIMIT" + to_string (res_page + 1));

  package_name prj;
  package_name pkg;
  size_t n (0);

  for (package& p: package_db_->query<package> (q))
  {
    if (!p.id.tenant.empty ())
      throw invalid_request (501, "not implemented");

    if (n++ == res_page)
    {
      if (p.project == prj)
      {
        s << ~DIV; // 'versions' class.

        if (p.name == pkg)
          s << DIV(ID="package-break") << "..." << ~DIV;

        s << DIV(ID="project-break") << "..." << ~DIV;

        // Make sure we don't serialize ~DIV(CLASS="versions") twice (see
        // below).
        //
        pkg = package_name ();
      }

      break;
    }

    if (p.project != prj)
    {
      if (!pkg.empty ())
        s << ~DIV; // 'versions' class.

      prj = move (p.project);
      pkg = package_name ();

      s << TABLE(CLASS="proplist project")
        <<   TBODY
        <<     TR_PROJECT (prj, root, tenant)
        <<   ~TBODY
        << ~TABLE;
    }

    if (p.name != pkg)
    {
      if (!pkg.empty ())
        s << ~DIV; // 'versions' class.

      pkg = move (p.name);

      s << TABLE(CLASS="proplist package")
        <<   TBODY
        <<     TR_NAME (pkg, root, p.tenant)
        <<     TR_SUMMARY (p.summary)
        <<     TR_LICENSE (p.license_alternatives)
        <<   ~TBODY
        << ~TABLE
        << DIV(CLASS="versions");
    }

    s << TABLE(CLASS="proplist version")
      <<   TBODY
      <<     TR_VERSION (pkg, p.version, root, tenant, p.upstream_version);

    assert (p.internal ());

    const repository_location& rl (p.internal_repository.load ()->location);

    s <<     TR_REPOSITORY (rl, root, tenant)
      <<     TR_DEPENDS (p.dependencies, root, tenant)
      <<     TR_REQUIRES (p.requirements);

    if (options_->reviews_url_specified ())
    {
      package_db_->load (p, p.reviews_section);

      s << TR_REVIEWS_SUMMARY (p.reviews, options_->reviews_url ());
    }

    s <<   ~TBODY
      << ~TABLE;
  }

  if (!pkg.empty ())
    s << ~DIV; // 'versions' class.

  t.commit ();

  string u (root.string () + "?advanced-search");

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
  add_filter ("pr", params.project ());
  add_filter ("rp", params.repository (), "*");
  add_filter ("rv", params.reviews (), "*");

  s <<       DIV_PAGER (page,
                        count,
                        res_page,
                        options_->search_pages (),
                        u)
    <<     ~DIV
    <<   ~BODY
    << ~HTML;

  return true;
}
