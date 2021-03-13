// file      : libbrep/build.cxx -*- C++ -*-
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
    case build_state::building: return "building";
    case build_state::built:    return "built";
    }

    return string (); // Should never reach.
  }

  build_state
  to_build_state (const string& s)
  {
         if (s == "building") return build_state::building;
    else if (s == "built")    return build_state::built;
    else throw invalid_argument ("invalid build state '" + s + "'");
  }

  // force_state
  //
  string
  to_string (force_state s)
  {
    switch (s)
    {
    case force_state::unforced: return "unforced";
    case force_state::forcing:  return "forcing";
    case force_state::forced:   return "forced";
    }

    return string (); // Should never reach.
  }

  force_state
  to_force_state (const string& s)
  {
         if (s == "unforced") return force_state::unforced;
    else if (s == "forcing")  return force_state::forcing;
    else if (s == "forced")   return force_state::forced;
    else throw invalid_argument ("invalid force state '" + s + "'");
  }

  // build
  //
  build::
  build (string tnt,
         package_name_type pnm,
         version pvr,
         string cfg,
         string tnm, version tvr,
         optional<string> inr,
         optional<string> afp, optional<string> ach,
         string mnm, string msm,
         butl::target_triplet trg)
      : id (package_id (move (tnt), move (pnm), pvr),
            move (cfg),
            move (tnm), tvr),
        tenant (id.package.tenant),
        package_name (id.package.name),
        package_version (move (pvr)),
        configuration (id.configuration),
        toolchain_name (id.toolchain_name),
        toolchain_version (move (tvr)),
        state (build_state::building),
        interactive (move (inr)),
        timestamp (timestamp_type::clock::now ()),
        force (force_state::unforced),
        agent_fingerprint (move (afp)), agent_challenge (move (ach)),
        machine (move (mnm)),
        machine_summary (move (msm)),
        target (move (trg))
  {
  }

  // build_delay
  //
  build_delay::
  build_delay (string tnt,
               package_name_type pnm, version pvr,
               string cfg,
               string tnm, version tvr,
               timestamp ptm)
      : id (package_id (move (tnt), move (pnm), pvr),
            move (cfg),
            move (tnm), tvr),
        tenant (id.package.tenant),
        package_name (id.package.name),
        package_version (move (pvr)),
        configuration (id.configuration),
        toolchain_name (id.toolchain_name),
        toolchain_version (move (tvr)),
        package_timestamp (ptm)
  {
  }
}
