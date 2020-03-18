// file      : mod/mod-repository-details.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-repository-details.hxx>

#include <algorithm> // max()

#include <libstudxml/serializer.hxx>

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <libbutl/timestamp.mxx> // to_string()

#include <web/server/module.hxx>
#include <web/server/mime-url-encoding.hxx>

#include <web/xhtml/serialization.hxx>

#include <libbrep/package.hxx>
#include <libbrep/package-odb.hxx>

#include <mod/page.hxx>
#include <mod/module-options.hxx>

using namespace std;
using namespace odb::core;
using namespace brep::cli;

// While currently the user-defined copy constructor is not required (we don't
// need to deep copy nullptr's), it is a good idea to keep the placeholder
// ready for less trivial cases.
//
brep::repository_details::
repository_details (const repository_details& r)
    : database_module (r),
      options_ (r.initialized_ ? r.options_ : nullptr)
{
}

void brep::repository_details::
init (scanner& s)
{
  HANDLER_DIAG;

  options_ = make_shared<options::repository_details> (
    s, unknown_mode::fail, unknown_mode::fail);

  database_module::init (*options_, options_->package_db_retry ());

  if (options_->root ().empty ())
    options_->root (dir_path ("/"));
}

bool brep::repository_details::
handle (request& rq, response& rs)
{
  using namespace web::xhtml;

  HANDLER_DIAG;

  const dir_path& root (options_->root ());

  // Make sure no parameters passed.
  //
  try
  {
    name_value_scanner s (rq.parameters (1024));
    params::repository_details (s, unknown_mode::fail, unknown_mode::fail);
  }
  catch (const cli::exception& e)
  {
    throw invalid_request (400, e.what ());
  }

  static const string title ("About");
  xml::serializer s (rs.content (), title);

  s << HTML
    <<   HEAD
    <<     TITLE << title << ~TITLE
    <<     CSS_LINKS (path ("repository-details.css"), root)
    <<   ~HEAD
    <<   BODY
    <<     DIV_HEADER (options_->logo (), options_->menu (), root, tenant)
    <<     DIV(ID="content");

  transaction t (package_db_->begin ());

  using query = query<repository>;

  for (const auto& r:
         package_db_->query<repository> (
           (query::internal && query::id.tenant == tenant) +
           "ORDER BY" + query::priority))
  {
    //@@ Feels like a lot of trouble (e.g., id_attribute()) for very
    //   dubious value. A link to the package search page just for
    //   this repository would probably be more useful.
    //
    string id (html_id (r.canonical_name));
    s << H1(ID=id)
      <<   A(HREF="#" + web::mime_url_encode (id, false))
      <<     r.display_name
      <<   ~A
      << ~H1;

    if (r.summary)
      s << H2 << *r.summary << ~H2;

    // Cleanup the URL fragment, if present.
    //
    repository_url u (r.location.url ());
    u.fragment = nullopt;

    s << P
      << A(HREF=u.string ()) << r.location << ~A << *BR;

    if (r.email)
    {
      const email& e (*r.email);

      s <<   A(HREF="mailto:" + e) << e << ~A;

      if (!e.comment.empty ())
        s << " (" << e.comment << ")";

      s << *BR;
    }

    s << butl::to_string (max (r.packages_timestamp, r.repositories_timestamp),
                          "%Y-%m-%d %H:%M:%S%[.N] %Z",
                          true,
                          true)
      << ~P;

    if (r.description)
      s << P_TEXT (*r.description);

    if (r.certificate)
    {
      const certificate& cert (*r.certificate);

      size_t np (cert.name.find (':'));
      assert (np != string::npos); // Naming scheme should always be present.

      // Mimic the suggested format of the repository description so that the
      // certificate info looks like just another section. Inside use the
      // format similar to the bpkg rep-info output.
      //
      s << P << "REPOSITORY CERTIFICATE" << ~P
        << P
        <<   "CN=" << cert.name.c_str () + np + 1 << *BR
        <<   "O=" << cert.organization << *BR
        <<   email (cert.email)
        << ~P
        << P(CLASS="certfp") << cert.fingerprint << ~P
        << PRE(CLASS="certpem") << cert.pem << ~PRE;
    }
  }

  t.commit ();

  s <<     ~DIV
    <<   ~BODY
    << ~HTML;

  return true;
}
