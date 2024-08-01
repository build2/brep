// file      : tests/manifest/driver.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <ios>      // ios_base::failbit, ios_base::badbit
#include <iostream>

#include <libbutl/utility.hxx>             // operator<<(ostream,exception)
#include <libbutl/manifest-parser.hxx>
#include <libbutl/manifest-serializer.hxx>

#include <libbrep/review-manifest.hxx>

#undef NDEBUG
#include <cassert>

using namespace std;
using namespace butl;
using namespace brep;

// Usage: argv[0] (-r | -rl)
//
// Read and parse manifest from STDIN and serialize it to STDOUT. The
// following options specify the manifest type.
//
// -r  parse review manifest
// -rl parse review manifest list
//
int
main (int argc, char* argv[])
try
{
  assert (argc == 2);
  string opt (argv[1]);

  cin.exceptions  (ios_base::failbit | ios_base::badbit);
  cout.exceptions (ios_base::failbit | ios_base::badbit);

  manifest_parser     p (cin,  "stdin");
  manifest_serializer s (cout, "stdout");

  if (opt == "-r")
    review_manifest (p).serialize (s);
  else if (opt == "-rl")
    review_manifests (p).serialize (s);
  else
    assert (false);

  return 0;
}
catch (const manifest_parsing& e)
{
  cerr << e << endl;
  return 1;
}
catch (const manifest_serialization& e)
{
  cerr << e << endl;
  return 1;
}
