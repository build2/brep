// file      : mod/mod-ci-github.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-ci-github.hxx>

//#include <libbutl/manifest-parser.hxx>
#include <libbutl/manifest-serializer.hxx>

#include <mod/module-options.hxx>

using namespace std;
using namespace butl;
using namespace web;
using namespace brep::cli;

brep::ci_github::
ci_github (const ci_github& r)
    : handler (r)
{
}

void brep::ci_github::
init (scanner& s)
{
  options_ = make_shared<options::ci> (
    s, unknown_mode::fail, unknown_mode::fail);
}

bool brep::ci_github::
handle (request& /*rq*/, response& rs)
{
  using namespace bpkg;
  using namespace xhtml;

  // using parser        = manifest_parser;
  // using parsing       = manifest_parsing;
  using serializer    = manifest_serializer;
  // using serialization = manifest_serialization;

  HANDLER_DIAG;

  string request_id; // Will be set later.
  auto respond_manifest = [&rs, &request_id] (status_code status,
                                              const string& message) -> bool
  {
    serializer s (rs.content (status, "text/manifest;charset=utf-8"),
                  "response");

    s.next ("", "1");                      // Start of manifest.
    s.next ("status", to_string (status));
    s.next ("message", message);

    if (!request_id.empty ())
      s.next ("reference", request_id);

    s.next ("", "");                       // End of manifest.
    return true;
  };

  return respond_manifest (404, "XXX CI request submission disabled");
}
