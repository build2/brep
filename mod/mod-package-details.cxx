// file      : mod/mod-package-details.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-package-details>

#include <xml/serializer>

#include <odb/session.hxx>
#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <web/xhtml>
#include <web/module>
#include <web/mime-url-encoding>

#include <brep/package>
#include <brep/package-odb>

#include <mod/page>
#include <mod/options>

using namespace odb::core;
using namespace brep::cli;

// While currently the user-defined copy constructor is not required (we don't
// need to deep copy nullptr's), it is a good idea to keep the placeholder
// ready for less trivial cases.
//
brep::package_details::
package_details (const package_details& r)
    : database_module (r),
      options_ (r.initialized_ ? r.options_ : nullptr)
{
}

void brep::package_details::
init (scanner& s)
{
  MODULE_DIAG;

  options_ = make_shared<options::package_details> (
    s, unknown_mode::fail, unknown_mode::fail);

  database_module::init (*options_);

  if (options_->root ().empty ())
    options_->root (dir_path ("/"));
}

template <typename T>
static inline query<T>
search_params (const brep::string& n, const brep::string& q)
{
  using query = query<T>;

  return "(" +
    (q.empty ()
     ? query ("NULL")
     : "plainto_tsquery (" + query::_val (q) + ")") +
    "," +
    query::_val (n) +
    ")";
}

bool brep::package_details::
handle (request& rq, response& rs)
{
  using namespace web;
  using namespace web::xhtml;

  MODULE_DIAG;

  const size_t res_page (options_->search_results ());
  const dir_path& root (options_->root ());

  const string& name (*rq.path ().rbegin ());
  const string ename (mime_url_encode (name));

  params::package_details params;
  bool full;

  try
  {
    name_value_scanner s (rq.parameters ());
    params = params::package_details (
      s, unknown_mode::fail, unknown_mode::fail);

    full = params.form () == page_form::full;
  }
  catch (const cli::exception& e)
  {
    throw invalid_request (400, e.what ());
  }

  size_t page (params.page ());
  const string& squery (params.query ());

  auto url = [&ename] (bool f = false,
                       const string& q = "",
                       size_t p = 0,
                       const string& a = "") -> string
  {
    string s ("?");
    string u (ename);

    if (f)           { u += "?f=full"; s = "&"; }
    if (!q.empty ()) { u += s + "q=" +  mime_url_encode (q); s = "&"; }
    if (p > 0)       { u += s + "p=" + to_string (p); s = "&"; }
    if (!a.empty ()) { u += '#' + a; }
    return u;
  };

  xml::serializer s (rs.content (), name);

  s << HTML
    <<   HEAD
    <<     TITLE
    <<       name;

  if (!squery.empty ())
    s << " " << squery;

  s <<     ~TITLE
    <<     CSS_LINKS (path ("package-details.css"), root)
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

  if (full)
    s << CLASS("full");

  s <<       DIV(ID="heading")
    <<         H1 << A(HREF=url ()) << name << ~A << ~H1
    <<         A(HREF=url (!full, squery, page))
    <<           (full ? "[brief]" : "[full]")
    <<         ~A
    <<       ~DIV;

  session sn;
  transaction t (db_->begin ());

  shared_ptr<package> pkg;
  {
    latest_package lp;
    if (!db_->query_one<latest_package> (
          query<latest_package>(
            "(" + query<latest_package>::_val (name) + ")"), lp))
      throw invalid_request (404, "Package '" + name + "' not found");

    pkg = db_->load<package> (lp.id);
  }

  const auto& licenses (pkg->license_alternatives);

  if (page == 0)
  {
    // Display package details on the first page only.
    //
    s << H2 << pkg->summary << ~H2;

    static const string id ("description");
    if (const auto& d = pkg->description)
      s << (full
            ? P_DESCRIPTION (*d, id)
            : P_DESCRIPTION (*d, options_->package_description (),
                             url (!full, squery, page, id)));

    s << TABLE(CLASS="proplist", ID="package")
      <<   TBODY
      <<     TR_LICENSE (licenses)
      <<     TR_URL (pkg->url)
      <<     TR_EMAIL (pkg->email)
      <<     TR_TAGS (pkg->tags, root)
      <<   ~TBODY
      << ~TABLE;
  }

  auto pkg_count (
    db_->query_value<package_count> (
      search_params<package_count> (name, squery)));

  s << FORM_SEARCH (squery)
    << DIV_COUNTER (pkg_count, "Version", "Versions");

  // Enclose the subsequent tables to be able to use nth-child CSS selector.
  //
  s << DIV;
  for (const auto& pr:
         db_->query<package_search_rank> (
           search_params<package_search_rank> (name, squery) +
           "ORDER BY rank DESC, version_epoch DESC, "
           "version_canonical_upstream DESC, version_canonical_release DESC, "
           "version_revision DESC" +
           "OFFSET" + to_string (page * res_page) +
           "LIMIT" + to_string (res_page)))
  {
    shared_ptr<package> p (db_->load<package> (pr.id));

    s << TABLE(CLASS="proplist version")
      <<   TBODY
      <<     TR_VERSION (name, p->version, root)

      // @@ Shouldn't we skip low priority row ? Don't think so, why?
      //
      <<     TR_PRIORITY (p->priority);

    // Comparing objects of the license_alternatives class as being of the
    // vector<vector<string>> class, so comments are not considered.
    //
    if (p->license_alternatives != licenses)
      s << TR_LICENSE (p->license_alternatives);

    assert (p->internal ());

    // @@ Shouldn't we make package repository name to be a link to the proper
    //    place of the About page, describing corresponding repository?
    //    Yes, I think that's sounds reasonable.
    //    Or maybe it can be something more valuable like a link to the
    //    repository package search page ?
    //
    // @@ In most cases package location will be the same for all versions
    //    of the same package. Shouldn't we put package location to the
    //    package summary part and display it here only if it differs
    //    from the one in the summary ?
    //
    //    Hm, I am not so sure about this. Consider: stable/testing/unstable.
    //
    s <<     TR_REPOSITORY (p->internal_repository.object_id (), root)
      <<     TR_DEPENDS (p->dependencies, root)
      <<     TR_REQUIRES (p->requirements)
      <<   ~TBODY
      << ~TABLE;
  }
  s << ~DIV;

  t.commit ();

  s <<       DIV_PAGER (page, pkg_count, res_page, options_->search_pages (),
                        url (full, squery))
    <<     ~DIV
    <<   ~BODY
    << ~HTML;

  return true;
}
