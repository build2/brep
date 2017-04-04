// file      : brep/build.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/build>

namespace brep
{
  // build_state
  //
  string
  to_string (build_state s)
  {
    switch (s)
    {
    case build_state::untested: return "untested";
    case build_state::testing:  return "testing";
    case build_state::tested:   return "tested";
    }

    return string (); // Should never reach.
  }

  build_state
  to_build_state (const string& s)
  {
         if (s == "untested") return build_state::untested;
    else if (s == "testing")  return build_state::testing;
    else if (s == "tested")   return build_state::tested;
    else throw invalid_argument ("invalid build state '" + s + "'");
  }

  // build
  //
  build::
  build (string pnm, version pvr, string cfg)
      : id (package_id (move (pnm), pvr), move (cfg)),
        package_name (id.package.name),
        package_version (move (pvr)),
        configuration (id.configuration),
        state (build_state::testing),
        timestamp (timestamp_type::clock::now ())
  {
  }
}
