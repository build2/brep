// file      : libbrep/build.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbrep/build.hxx>

namespace brep
{
  // build_state
  //
  string
  to_string (build_state s)
  {
    switch (s)
    {
    case build_state::unbuilt:  return "unbuilt";
    case build_state::building: return "building";
    case build_state::built:    return "built";
    }

    return string (); // Should never reach.
  }

  build_state
  to_build_state (const string& s)
  {
         if (s == "unbuilt")  return build_state::unbuilt;
    else if (s == "building") return build_state::building;
    else if (s == "built")    return build_state::built;
    else throw invalid_argument ("invalid build state '" + s + "'");
  }

  // build
  //
  build::
  build (string pnm, version pvr,
         string cfg,
         string tnm, version tvr,
         string mnm, string msm,
         optional<butl::target_triplet> trg)
      : id (package_id (move (pnm), pvr), move (cfg), tvr),
        package_name (id.package.name),
        package_version (move (pvr)),
        configuration (id.configuration),
        toolchain_name (move (tnm)),
        toolchain_version (move (tvr)),
        state (build_state::building),
        timestamp (timestamp_type::clock::now ()),
        forced (false),
        machine (move (mnm)),
        machine_summary (move (msm)),
        target (move (trg))
  {
  }
}
