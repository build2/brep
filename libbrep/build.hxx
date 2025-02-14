// file      : libbrep/build.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBREP_BUILD_HXX
#define LIBBREP_BUILD_HXX

#include <chrono>

#include <odb/core.hxx>
#include <odb/section.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <libbrep/common.hxx>
#include <libbrep/build-package.hxx>

// Must be included after libbrep/common.hxx, so that the _version structure
// get defined before libbpkg/manifest.hxx inclusion.
//
// Note that if we start using assert() in get/set expressions in this header,
// we will have to redefine it for ODB compiler after all include directives
// (see libbrep/common.hxx for details).
//
#include <libbbot/manifest.hxx>

// Used by the data migration entries.
//
#define LIBBREP_BUILD_SCHEMA_VERSION_BASE 28

#pragma db model version(LIBBREP_BUILD_SCHEMA_VERSION_BASE, 29, closed)

// We have to keep these mappings at the global scope instead of inside the
// brep namespace because they need to be also effective in the bbot namespace
// from which we "borrow" types (and some of them use the mapped types).
//
#pragma db map type(bbot::result_status) as(std::string) \
  to(to_string (?))                                      \
  from(bbot::to_result_status (?))

namespace brep
{
  #pragma db value
  struct build_id
  {
    package_id package;
    target_triplet target;
    string target_config_name;
    string package_config_name;
    string toolchain_name;
    canonical_version toolchain_version;

    build_id () = default;
    build_id (package_id p,
              target_triplet t,
              string tc,
              string pc,
              string n,
              const brep::version& v)
        : package (move (p)),
          target (move (t)),
          target_config_name (move (tc)),
          package_config_name (move (pc)),
          toolchain_name (move (n)),
          toolchain_version (v) {}
  };

  inline bool
  operator< (const build_id& x, const build_id& y)
  {
    if (x.package != y.package)
      return x.package < y.package;

    if (int r = x.target.compare (y.target))
      return r < 0;

    if (int r = x.target_config_name.compare (y.target_config_name))
      return r < 0;

    if (int r = x.package_config_name.compare (y.package_config_name))
      return r < 0;

    if (int r = x.toolchain_name.compare (y.toolchain_name))
      return r < 0;

    return compare_version_lt (x.toolchain_version, y.toolchain_version, true);
  }

  // These allow comparing objects that have package, configuration, target,
  // toolchain_name, and toolchain_version data members to build_id values.
  // The idea is that this works for both query members of build id types as
  // well as for values of the build_id type.
  //
  template <typename T>
  inline auto
  operator== (const T& x, const build_id& y)
    -> decltype (x.package == y.package                         &&
                 x.target == y.target                           &&
                 x.target_config_name == y.target_config_name   &&
                 x.package_config_name == y.package_config_name &&
                 x.toolchain_name == y.toolchain_name           &&
                 x.toolchain_version.epoch == y.toolchain_version.epoch)
  {
    return x.package == y.package                         &&
           x.target == y.target                           &&
           x.target_config_name == y.target_config_name   &&
           x.package_config_name == y.package_config_name &&
           x.toolchain_name == y.toolchain_name           &&
           compare_version_eq (x.toolchain_version, y.toolchain_version, true);
  }

  template <typename T>
  inline auto
  operator!= (const T& x, const build_id& y)
    -> decltype (x.package == y.package                         &&
                 x.target == y.target                           &&
                 x.target_config_name == y.target_config_name   &&
                 x.package_config_name == y.package_config_name &&
                 x.toolchain_name == y.toolchain_name           &&
                 x.toolchain_version.epoch == y.toolchain_version.epoch)
  {
    return x.package != y.package                         ||
           x.target != y.target                           ||
           x.target_config_name != y.target_config_name   ||
           x.package_config_name != y.package_config_name ||
           x.toolchain_name != y.toolchain_name           ||
           compare_version_ne (x.toolchain_version, y.toolchain_version, true);
  }

  // Allow comparing the query members with the query parameters bound by
  // reference to variables of the build id type (in particular in the
  // prepared queries).
  //
  // Note that it is not operator==() since the query template parameter type
  // can not be deduced from the function parameter types and needs to be
  // specified explicitly.
  //
  template <typename T, typename ID>
  inline auto
  equal (const ID& x, const build_id& y, bool toolchain_version = true)
    -> decltype (x.package.tenant == odb::query<T>::_ref (y.package.tenant) &&
                 x.package.name == odb::query<T>::_ref (y.package.name)     &&
                 x.package.version.epoch ==
                   odb::query<T>::_ref (y.package.version.epoch)            &&
                 x.target_config_name ==
                   odb::query<T>::_ref (y.target_config_name)               &&
                 x.toolchain_name == odb::query<T>::_ref (y.toolchain_name) &&
                 x.toolchain_version.epoch ==
                   odb::query<T>::_ref (y.toolchain_version.epoch))
  {
    using query = odb::query<T>;

    query r (equal<T> (x.package, y.package)                              &&
             x.target              == query::_ref (y.target)              &&
             x.target_config_name  == query::_ref (y.target_config_name)  &&
             x.package_config_name == query::_ref (y.package_config_name) &&
             x.toolchain_name      == query::_ref (y.toolchain_name));

    if (toolchain_version)
      r = r && equal<T> (x.toolchain_version, y.toolchain_version);

    return r;
  }

  // build_state
  //
  // The queued build state is semantically equivalent to a non-existent
  // build. It is only used for those tenants, which have a third-party
  // service associated that requires the `queued` notifications (see
  // mod/tenant-service.hxx for background).
  //
  enum class build_state: std::uint8_t
  {
    queued,
    building,
    built
  };

  string
  to_string (build_state);

  build_state
  to_build_state (const string&); // May throw invalid_argument.

  inline ostream&
  operator<< (ostream& os, build_state s) {return os << to_string (s);}

  #pragma db map type(build_state) as(string) \
    to(to_string (?))                         \
    from(brep::to_build_state (?))

  // force_state
  //
  enum class force_state: std::uint8_t
  {
    unforced,
    forcing,  // Rebuild is forced while being in the building state.
    forced    // Rebuild is forced while being in the built state.
  };

  string
  to_string (force_state);

  force_state
  to_force_state (const string&); // May throw invalid_argument.

  inline ostream&
  operator<< (ostream& os, force_state s) {return os << to_string (s);}

  #pragma db map type(force_state) as(string) \
    to(to_string (?))                         \
    from(brep::to_force_state (?))

  // result_status
  //
  using bbot::result_status;

  using optional_result_status = optional<result_status>;

  #pragma db map type(optional_result_status) as(optional_string) \
    to((?) ? bbot::to_string (*(?)) : brep::optional_string ())   \
    from((?)                                                      \
         ? bbot::to_result_status (*(?))                          \
         : brep::optional_result_status ())

  // operation_results
  //
  using bbot::operation_result;
  #pragma db value(operation_result) definition

  using bbot::operation_results;

  #pragma db value
  struct build_machine
  {
    string name;
    string summary;
  };

  #pragma db object pointer(shared_ptr) session
  class build
  {
  public:
    using timestamp_type    = brep::timestamp;
    using package_name_type = brep::package_name;

    // Create the build object with the building state, non-existent status,
    // the timestamp set to now, and the force state set to unforced.
    //
    build (string tenant,
           package_name_type, version,
           target_triplet,
           string target_config_name,
           string package_config_name,
           string toolchain_name, version toolchain_version,
           optional<string> interactive,
           optional<string> agent_fingerprint,
           optional<string> agent_challenge,
           build_machine,
           vector<build_machine> auxiliary_machines,
           string controller_checksum,
           string machine_checksum);

    // Create the build object with the queued state.
    //
    build (string tenant,
           package_name_type, version,
           target_triplet,
           string target_config_name,
           string package_config_name,
           string toolchain_name, version toolchain_version);

    // Create the build object with the built state, the specified status and
    // operation results, all the timestamps set to now, and the force state
    // set to unforced.
    //
    build (string tenant,
           package_name_type, version,
           target_triplet,
           string target_config_name,
           string package_config_name,
           string toolchain_name, version toolchain_version,
           result_status,
           operation_results,
           build_machine,
           vector<build_machine> auxiliary_machines = {});

    // Move-only type.
    //
    build (build&&);
    build& operator= (build&&);

    build (const build&) = delete;
    build& operator= (const build&) = delete;

    build_id id;

    string& tenant;                     // Tracks id.package.tenant.
    package_name_type& package_name;    // Tracks id.package.name.
    upstream_version package_version;   // Original of id.package.version.
    target_triplet& target;             // Tracks id.target.
    string& target_config_name;         // Tracks id.target_config_name.
    string& package_config_name;        // Tracks id.package_config_name.
    string& toolchain_name;             // Tracks id.toolchain_name.
    upstream_version toolchain_version; // Original of id.toolchain_version.

    build_state state;

    // If present, the login information for the interactive build. May be
    // present only in the building state.
    //
    optional<string> interactive;

    // Time of the last state change (the creation time initially).
    //
    timestamp_type timestamp;

    force_state force;

    // Must be present for the built state, may be present for the building
    // state.
    //
    optional<result_status> status;

    // Times of the last soft/hard completed (re)builds. Used to decide when
    // to perform soft and hard rebuilds, respectively.
    //
    // The soft timestamp is updated whenever we receive a task result.
    //
    // The hard timestamp is updated whenever we receive a task result with
    // a status other than skip.
    //
    // Also note that whenever hard_timestamp is updated, soft_timestamp is
    // updated as well and whenever soft_timestamp is updated, timestamp is
    // updated as well. Thus the following condition is always true:
    //
    // hard_timestamp <= soft_timestamp <= timestamp
    //
    // Note that the "completed" above means that we may analyze the task
    // result/log and deem it as not completed and proceed with automatic
    // rebuild (the flake monitor idea).
    //
    timestamp_type soft_timestamp;
    timestamp_type hard_timestamp;

    // May be present only for the building state.
    //
    optional<string> agent_fingerprint;
    optional<string> agent_challenge;

    build_machine machine;
    vector<build_machine> auxiliary_machines;
    odb::section auxiliary_machines_section;

    // Note that the logs are stored as std::string/TEXT which is Ok since
    // they are UTF-8 and our database is UTF-8.
    //
    operation_results results;
    odb::section results_section;

    // Checksums of entities involved in the build.
    //
    // Optional checksums are provided by the external entities (agent and
    // worker). All are absent initially.
    //
    // Note that the agent checksum can also be absent after the hard rebuild
    // task is issued and the worker and dependency checksums - after a failed
    // rebuild (error result status or worse).
    //
    string           controller_checksum;
    string           machine_checksum;
    optional<string> agent_checksum;
    optional<string> worker_checksum;
    optional<string> dependency_checksum;

    // Database mapping.
    //
    #pragma db member(id) id column("")

    #pragma db member(tenant) transient
    #pragma db member(package_name) transient
    #pragma db member(package_version) \
      set(this.package_version.init (this.id.package.version, (?)))
    #pragma db member(target) transient
    #pragma db member(target_config_name) transient
    #pragma db member(package_config_name) transient
    #pragma db member(toolchain_name) transient
    #pragma db member(toolchain_version) \
      set(this.toolchain_version.init (this.id.toolchain_version, (?)))

    // Speed-up queries with ordering the result by the timestamp.
    //
    #pragma db member(timestamp) index

    #pragma db member(machine) transient

    #pragma db member(machine_name) virtual(std::string) \
      access(machine.name) column("machine")

    #pragma db member(machine_summary) virtual(std::string) \
      access(machine.summary)

    #pragma db member(auxiliary_machines) id_column("") value_column("") \
      section(auxiliary_machines_section)

    #pragma db member(auxiliary_machines_section) load(lazy) update(always)

    #pragma db member(results) id_column("") value_column("") \
      section(results_section)

    #pragma db member(results_section) load(lazy) update(always)

  private:
    friend class odb::access;

    build ()
        : tenant (id.package.tenant),
          package_name (id.package.name),
          target (id.target),
          target_config_name (id.target_config_name),
          package_config_name (id.package_config_name),
          toolchain_name (id.toolchain_name) {}
  };

  // Note that ADL can't find the equal operator in join conditions, so we use
  // the function call notation for them.
  //

  // Toolchains of existing buildable package builds.
  //
  #pragma db view object(build)                                       \
    object(build_package inner:                                       \
           brep::operator== (build::id.package, build_package::id) && \
           build_package::buildable)                                  \
    query(distinct)
  struct toolchain
  {
    string name;
    upstream_version version;

    // Database mapping. Note that the version member must be loaded after
    // the virtual members since the version_ member must filled by that time.
    //
    #pragma db member(name) column(build::id.toolchain_name)

    #pragma db member(version) column(build::toolchain_version) \
      set(this.version.init (this.version_, (?)))

    #pragma db member(epoch) virtual(uint16_t)  \
      before(version) access(version_.epoch)    \
      column(build::id.toolchain_version.epoch)

    #pragma db member(canonical_upstream) virtual(std::string) \
      before(version) access(version_.canonical_upstream)      \
      column(build::id.toolchain_version.canonical_upstream)

    #pragma db member(canonical_release) virtual(std::string) \
      before(version) access(version_.canonical_release)      \
      column(build::id.toolchain_version.canonical_release)

    #pragma db member(revision) virtual(uint16_t)  \
      before(version) access(version_.revision)    \
      column(build::id.toolchain_version.revision)

  private:
    friend class odb::access;

    #pragma db transient
    canonical_version version_;
  };

  // Builds of existing buildable packages.
  //
  #pragma db view                                                      \
    object(build)                                                      \
    object(build_package inner:                                        \
           brep::operator== (build::id.package, build_package::id) &&  \
           build_package::buildable)                                   \
    object(build_tenant: build_package::id.tenant == build_tenant::id)
  struct package_build
  {
    shared_ptr<brep::build> build;
    bool archived; // True if the tenant the build belongs to is archived.
  };

  #pragma db view                                                      \
    object(build)                                                      \
    object(build_package inner:                                        \
           brep::operator== (build::id.package, build_package::id) &&  \
           build_package::buildable)                                   \
    object(build_tenant: build_package::id.tenant == build_tenant::id)
  struct package_build_count
  {
    size_t result;

    operator size_t () const {return result;}

    // Database mapping.
    //
    #pragma db member(result) column("count(" + build::id.package.name + ")")
  };

  // Ids of existing buildable package builds.
  //
  #pragma db view object(build)                                       \
    object(build_package inner:                                       \
           brep::operator== (build::id.package, build_package::id) && \
           build_package::buildable)
  struct package_build_id
  {
    build_id id;

    operator build_id& () {return id;}
  };

  // Used to track the package build delays since the last build or, if not
  // present, since the first opportunity to build the package.
  //
  #pragma db object pointer(shared_ptr) session
  class build_delay
  {
  public:
    using package_name_type = brep::package_name;

    // If toolchain version is empty, then the object represents a minimum
    // delay across all versions of the toolchain.
    //
    build_delay (string tenant,
                 package_name_type, version,
                 target_triplet,
                 string target_config_name,
                 string package_config_name,
                 string toolchain_name, version toolchain_version,
                 timestamp package_timestamp);

    build_id id;

    string& tenant;                     // Tracks id.package.tenant.
    package_name_type& package_name;    // Tracks id.package.name.
    upstream_version package_version;   // Original of id.package.version.
    target_triplet& target;             // Tracks id.target.
    string& target_config_name;         // Tracks id.target_config_name.
    string& package_config_name;        // Tracks id.package_config_name.
    string& toolchain_name;             // Tracks id.toolchain_name.
    upstream_version toolchain_version; // Original of id.toolchain_version.

    // Times of the latest soft and hard rebuild delay reports. Initialized
    // with timestamp_nonexistent by default.
    //
    // Note that both reports notify about initial build delays (at their
    // respective time intervals).
    //
    timestamp report_soft_timestamp;
    timestamp report_hard_timestamp;

    // Time when the package is initially considered as buildable for this
    // configuration and toolchain. It is used to track the build delay if the
    // build object is absent (the first build task is not yet issued, the
    // build is removed by brep-clean, etc).
    //
    timestamp package_timestamp;

    // Database mapping.
    //
    #pragma db member(id) id column("")

    #pragma db member(tenant) transient
    #pragma db member(package_name) transient
    #pragma db member(package_version) \
      set(this.package_version.init (this.id.package.version, (?)))
    #pragma db member(target) transient
    #pragma db member(target_config_name) transient
    #pragma db member(package_config_name) transient
    #pragma db member(toolchain_name) transient
    #pragma db member(toolchain_version) \
      set(this.toolchain_version.init (this.id.toolchain_version, (?)))

  private:
    friend class odb::access;

    build_delay ()
        : tenant (id.package.tenant),
          package_name (id.package.name),
          target (id.target),
          target_config_name (id.target_config_name),
          package_config_name (id.package_config_name),
          toolchain_name (id.toolchain_name) {}
  };
}

#endif // LIBBREP_BUILD_HXX
