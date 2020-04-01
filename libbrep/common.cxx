// file      : libbrep/common.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbrep/common.hxx>

namespace brep
{
  const version wildcard_version (0, "0", nullopt, nullopt, 0);

  // buildable_status
  //
  string
  to_string (buildable_status s)
  {
    switch (s)
    {
    case buildable_status::buildable:   return "buildable";
    case buildable_status::unbuildable: return "unbuildable";
    case buildable_status::external:    return "external";
    case buildable_status::test:        return "test";
    case buildable_status::stub:        return "stub";
    }

    return string (); // Should never reach.
  }

  buildable_status
  to_buildable_status (const string& s)
  {
         if (s == "buildable")   return buildable_status::buildable;
    else if (s == "unbuildable") return buildable_status::unbuildable;
    else if (s == "external")    return buildable_status::external;
    else if (s == "test")        return buildable_status::test;
    else if (s == "stub")        return buildable_status::stub;
    else throw invalid_argument ("invalid buildable status '" + s + "'");
  }
}
