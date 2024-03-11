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
    case build_state::queued:   return "queued";
    case build_state::building: return "building";
    case build_state::built:    return "built";
    }

    return string (); // Should never reach.
  }

  build_state
  to_build_state (const string& s)
  {
         if (s == "queued")   return build_state::queued;
    else if (s == "building") return build_state::building;
    else if (s == "built")    return build_state::built;
    else throw invalid_argument ("invalid build state '" + s + '\'');
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
    else throw invalid_argument ("invalid force state '" + s + '\'');
  }

  // build
  //
  build::
  build (string tnt,
         package_name_type pnm,
         version pvr,
         target_triplet trg,
         string tcf,
         string pcf,
         string tnm, version tvr,
         optional<string> inr,
         optional<string> afp, optional<string> ach,
         build_machine mcn,
         vector<build_machine> ams,
         string ccs,
         string mcs)
      : id (package_id (move (tnt), move (pnm), pvr),
            move (trg),
            move (tcf),
            move (pcf),
            move (tnm), tvr),
        tenant (id.package.tenant),
        package_name (id.package.name),
        package_version (move (pvr)),
        target (id.target),
        target_config_name (id.target_config_name),
        package_config_name (id.package_config_name),
        toolchain_name (id.toolchain_name),
        toolchain_version (move (tvr)),
        state (build_state::building),
        interactive (move (inr)),
        timestamp (timestamp_type::clock::now ()),
        force (force_state::unforced),
        agent_fingerprint (move (afp)), agent_challenge (move (ach)),
        machine (move (mcn)),
        auxiliary_machines (move (ams)),
        controller_checksum (move (ccs)),
        machine_checksum (move (mcs))
  {
  }

  build::
  build (string tnt,
         package_name_type pnm,
         version pvr,
         target_triplet trg,
         string tcf,
         string pcf,
         string tnm, version tvr)
      : id (package_id (move (tnt), move (pnm), pvr),
            move (trg),
            move (tcf),
            move (pcf),
            move (tnm), tvr),
        tenant (id.package.tenant),
        package_name (id.package.name),
        package_version (move (pvr)),
        target (id.target),
        target_config_name (id.target_config_name),
        package_config_name (id.package_config_name),
        toolchain_name (id.toolchain_name),
        toolchain_version (move (tvr)),
        state (build_state::queued),
        timestamp (timestamp_type::clock::now ()),
        force (force_state::unforced)
  {
  }

  build::
  build (string tnt,
         package_name_type pnm,
         version pvr,
         target_triplet trg,
         string tcf,
         string pcf,
         string tnm, version tvr,
         result_status rst,
         operation_results ors,
         build_machine mcn,
         vector<build_machine> ams)
      : id (package_id (move (tnt), move (pnm), pvr),
            move (trg),
            move (tcf),
            move (pcf),
            move (tnm), tvr),
        tenant (id.package.tenant),
        package_name (id.package.name),
        package_version (move (pvr)),
        target (id.target),
        target_config_name (id.target_config_name),
        package_config_name (id.package_config_name),
        toolchain_name (id.toolchain_name),
        toolchain_version (move (tvr)),
        state (build_state::built),
        timestamp (timestamp_type::clock::now ()),
        force (force_state::unforced),
        status (rst),
        soft_timestamp (timestamp),
        hard_timestamp (timestamp),
        machine (move (mcn)),
        auxiliary_machines (move (ams)),
        results (move (ors))
  {
  }

  build::
  build (build&& b)
      : id (move (b.id)),
        tenant (id.package.tenant),
        package_name (id.package.name),
        package_version (move (b.package_version)),
        target (id.target),
        target_config_name (id.target_config_name),
        package_config_name (id.package_config_name),
        toolchain_name (id.toolchain_name),
        toolchain_version (move (b.toolchain_version)),
        state (b.state),
        interactive (move (b.interactive)),
        timestamp (b.timestamp),
        force (b.force),
        status (b.status),
        soft_timestamp (b.soft_timestamp),
        hard_timestamp (b.hard_timestamp),
        agent_fingerprint (move (b.agent_fingerprint)),
        agent_challenge (move (b.agent_challenge)),
        machine (move (b.machine)),
        auxiliary_machines (move (b.auxiliary_machines)),
        auxiliary_machines_section (move (b.auxiliary_machines_section)),
        results (move (b.results)),
        results_section (move (b.results_section)),
        controller_checksum (move (b.controller_checksum)),
        machine_checksum (move (b.machine_checksum)),
        agent_checksum (move (b.agent_checksum)),
        worker_checksum (move (b.worker_checksum)),
        dependency_checksum (move (b.dependency_checksum))
  {
  }

  build& build::
  operator= (build&& b)
  {
    if (this != &b)
    {
      id = move (b.id);
      package_version = move (b.package_version);
      toolchain_version = move (b.toolchain_version);
      state = b.state;
      interactive = move (b.interactive);
      timestamp = b.timestamp;
      force = b.force;
      status = b.status;
      soft_timestamp = b.soft_timestamp;
      hard_timestamp = b.hard_timestamp;
      agent_fingerprint = move (b.agent_fingerprint);
      agent_challenge = move (b.agent_challenge);
      machine = move (b.machine);
      auxiliary_machines = move (b.auxiliary_machines);
      auxiliary_machines_section = move (b.auxiliary_machines_section);
      results = move (b.results);
      results_section = move (b.results_section);
      controller_checksum = move (b.controller_checksum);
      machine_checksum = move (b.machine_checksum);
      agent_checksum = move (b.agent_checksum);
      worker_checksum = move (b.worker_checksum);
      dependency_checksum = move (b.dependency_checksum);
    }

    return *this;
  }

  // build_delay
  //
  build_delay::
  build_delay (string tnt,
               package_name_type pnm, version pvr,
               target_triplet trg,
               string tcf,
               string pcf,
               string tnm, version tvr,
               timestamp ptm)
      : id (package_id (move (tnt), move (pnm), pvr),
            move (trg),
            move (tcf),
            move (pcf),
            move (tnm), tvr),
        tenant (id.package.tenant),
        package_name (id.package.name),
        package_version (move (pvr)),
        target (id.target),
        target_config_name (id.target_config_name),
        package_config_name (id.package_config_name),
        toolchain_name (id.toolchain_name),
        toolchain_version (move (tvr)),
        package_timestamp (ptm)
  {
  }
}
