// file      : mod/mod-packages.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-packages.hxx>

#include <libstudxml/serializer.hxx>

#include <odb/session.hxx>
#include <odb/database.hxx>
#include <odb/transaction.hxx>
#include <odb/schema-catalog.hxx>

#include <web/server/module.hxx>
#include <web/server/mime-url-encoding.hxx>

#include <web/xhtml/serialization.hxx>

#include <libbrep/package.hxx>
#include <libbrep/package-odb.hxx>

#include <mod/page.hxx>
#include <mod/module-options.hxx>

using namespace odb::core;
using namespace brep::cli;

// While currently the user-defined copy constructor is not required (we don't
// need to deep copy nullptr's), it is a good idea to keep the placeholder
// ready for less trivial cases.
//
brep::packages::
packages (const packages& r)
    : database_module (r),
      options_ (r.initialized_ ? r.options_ : nullptr)
{
}

void brep::packages::
init (scanner& s)
{
  HANDLER_DIAG;

  options_ = make_shared<options::packages> (
    s, unknown_mode::fail, unknown_mode::fail);

  database_module::init (*options_, options_->package_db_retry ());

  if (options_->root ().empty ())
    options_->root (dir_path ("/"));

  // Check that the database 'package' schema matches the current one. It's
  // enough to perform the check in just a single module implementation (and
  // we don't do in the dispatcher because it doesn't use the database).
  //
  // Note that the failure can be reported by each web server worker process.
  // While it could be tempting to move the check to the
  // repository_root::version() function, it would be wrong. The function can
  // be called by a different process (usually the web server root one) not
  // having the proper permissions to access the database.
  //
  const string ds ("package");
  if (schema_catalog::current_version (*package_db_, ds) !=
      package_db_->schema_version (ds))
    fail << "database 'package' schema differs from the current one (module "
         << BREP_VERSION_ID << ")";
}

template <typename T>
static inline query<T>
search_param (const brep::string& q, const brep::optional<brep::string>& t)
{
  using query = query<T>;
  return "(" +
    (q.empty ()
     ? query ("NULL")
     : "plainto_tsquery (" + query::_val (q) + ")") +
    "," +
    (!t ? query ("NULL") : query (query::_val (*t))) +
    ")";
}

bool brep::packages::
handle (request& rq, response& rs)
{
  using namespace web::xhtml;

  HANDLER_DIAG;

  const size_t res_page (options_->search_page_entries ());
  const dir_path& root (options_->root ());
  const string& title (options_->search_title ());
  const string& tenant_name (options_->tenant_name ());

  params::packages params;

  try
  {
    name_value_scanner s (rq.parameters (8 * 1024));
    params = params::packages (s, unknown_mode::fail, unknown_mode::fail);
  }
  catch (const cli::exception& e)
  {
    throw invalid_request (400, e.what ());
  }

  size_t page (params.page ());
  const string& squery (params.q ());
  string equery (web::mime_url_encode (squery));

  xml::serializer s (rs.content (), title);

  s << HTML
    <<   HEAD
    <<     TITLE
    <<       title;

  if (!squery.empty ())
    s << " " << squery;

  s <<     ~TITLE
    <<     CSS_LINKS (path ("packages.css"), root)
    //
    // This hack is required to avoid the "flash of unstyled content", which
    // happens due to the presence of the autofocus attribute in the input
    // element of the search form. The problem appears in Firefox and has a
    // (4-year old, at the time of this writing) bug report:
    //
    // https://bugzilla.mozilla.org/show_bug.cgi?id=712130
    //
    // @@ An update: claimed to be fixed in Firefox 60 that is released in
    //    May 2018. Is it time to cleanup? Remember to cleanup in all places.
    //
    <<     SCRIPT << " " << ~SCRIPT
    <<   ~HEAD
    <<   BODY
    <<     DIV_HEADER (options_->logo (), options_->menu (), root, tenant)
    <<     DIV(ID="content");

  // If the tenant is empty then we are in the global view and will display
  // packages from all the public tenants.
  //
  optional<string> tn;
  if (!tenant.empty ())
    tn = tenant;

  session sn;
  transaction t (package_db_->begin ());

  auto pkg_count (
    package_db_->query_value<latest_package_count> (
      search_param<latest_package_count> (squery, tn)));

  s << FORM_SEARCH (squery, "packages")
    << DIV_COUNTER (pkg_count, "Package", "Packages");

  // Enclose the subsequent tables to be able to use nth-child CSS selector.
  //
  s << DIV;
  for (const auto& pr:
         package_db_->query<latest_package_search_rank> (
           search_param<latest_package_search_rank> (squery, tn) +
           "ORDER BY rank DESC, name, tenant" +
           "OFFSET" + to_string (page * res_page) +
           "LIMIT" + to_string (res_page)))
  {
    shared_ptr<package> p (package_db_->load<package> (pr.id));

    s << TABLE(CLASS="proplist package")
      <<   TBODY
      <<     TR_NAME (p->name, equery, root, p->tenant)
      <<     TR_SUMMARY (p->summary)
      <<     TR_LICENSE (p->license_alternatives)
      <<     TR_DEPENDS (p->dependencies, root, p->tenant)
      <<     TR_REQUIRES (p->requirements);

    // In the global view mode add the tenant packages link. Note that the
    // global view (and the link) makes sense only in the multi-tenant mode.
    //
    if (!tn && !p->tenant.empty ())
      s << TR_TENANT (tenant_name, "packages", root, p->tenant);

    s <<   ~TBODY
      << ~TABLE;
  }
  s << ~DIV;

  t.commit ();

  string url (tenant_dir (root, tenant).string () + "?packages");

  if (!equery.empty ())
  {
    url += '=';
    url += equery;
  }

  s <<       DIV_PAGER (page,
                        pkg_count,
                        res_page,
                        options_->search_pages (),
                        url)
    <<     ~DIV
    <<   ~BODY
    << ~HTML;

  return true;
}
