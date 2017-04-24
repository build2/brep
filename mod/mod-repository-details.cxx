// file      : mod/mod-repository-details.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-repository-details>

#include <time.h> // tzset()

#include <sstream>
#include <algorithm> // max()

#include <xml/serializer>

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <butl/timestamp>

#include <web/xhtml>
#include <web/module>
#include <web/mime-url-encoding>

#include <brep/package>
#include <brep/package-odb>

#include <mod/page>
#include <mod/options>

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
  MODULE_DIAG;

  options_ = make_shared<options::repository_details> (
    s, unknown_mode::fail, unknown_mode::fail);

  database_module::init (*options_, options_->package_db_retry ());

  if (options_->root ().empty ())
    options_->root (dir_path ("/"));

  tzset (); // To use butl::to_stream() later on.
}

bool brep::repository_details::
handle (request& rq, response& rs)
{
  using namespace web::xhtml;

  MODULE_DIAG;

  const dir_path& root (options_->root ());

  // Make sure no parameters passed.
  //
  try
  {
    name_value_scanner s (rq.parameters ());
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

  transaction t (package_db_->begin ());

  using query = query<repository>;

  for (const auto& r:
         package_db_->query<repository> (
           query::internal + "ORDER BY" + query::priority))
  {
    //@@ Feels like a lot of trouble (e.g., id_attribute()) for very
    //   dubious value. A link to the package search page just for
    //   this repository would probably be more useful.
    //
    string id (html_id (r.name));
    s << H1(ID=id)
      <<   A(HREF="#" + web::mime_url_encode (id)) << r.display_name << ~A
      << ~H1;

    if (r.summary)
      s << H2 << *r.summary << ~H2;

    s << P
      << A(HREF=r.location.string ()) << r.location << ~A << *BR;

    if (r.email)
    {
      const email& e (*r.email);

      s <<   A(HREF="mailto:" + e) << e << ~A;

      if (!e.comment.empty ())
        s << " (" << e.comment << ")";

      s << *BR;
    }

    ostringstream o;
    butl::to_stream (o,
                     max (r.packages_timestamp, r.repositories_timestamp),
                     "%Y-%m-%d %H:%M:%S%[.N] %Z",
                     true,
                     true);

    s << o.str ()
      << ~P;

    if (r.description)
      s << P_DESCRIPTION (*r.description);

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
        << "CN=" << cert.name.c_str () + np + 1 << *BR
        << "O=" << cert.organization << *BR
        << email (cert.email)
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
