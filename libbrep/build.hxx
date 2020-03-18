// file      : libbrep/build.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBREP_BUILD_HXX
#define LIBBREP_BUILD_HXX

#include <chrono>

#include <odb/core.hxx>
#include <odb/section.hxx>

#include <libbutl/target-triplet.mxx>

#include <libbbot/manifest.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

// Must be included last (see assert in libbrep/common.hxx).
//
#include <libbrep/common.hxx>
#include <libbrep/build-package.hxx>

// Used by the data migration entries.
//
#define LIBBREP_BUILD_SCHEMA_VERSION_BASE 9

#pragma db model version(LIBBREP_BUILD_SCHEMA_VERSION_BASE, 10, closed)

// We have to keep these mappings at the global scope instead of inside
// the brep namespace because they need to be also effective in the
// bbot namespace from which we "borrow" types (and some of them use the mapped
// types).
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
    string configuration;
    string toolchain_name;
    canonical_version toolchain_version;

    build_id () = default;
    build_id (package_id p, string c, string n, const brep::version& v)
        : package (move (p)),
          configuration (move (c)),
          toolchain_name (move (n)),
          toolchain_version (v) {}
  };

  inline bool
  operator< (const build_id& x, const build_id& y)
  {
    if (x.package != y.package)
      return x.package < y.package;

    if (int r = x.configuration.compare (y.configuration))
      return r < 0;

    if (int r = x.toolchain_name.compare (y.toolchain_name))
      return r < 0;

    return compare_version_lt (x.toolchain_version, y.toolchain_version, true);
  }

  // These allow comparing objects that have package, configuration,
  // toolchain_name, and toolchain_version data members to build_id values.
  // The idea is that this works for both query members of build id types as
  // well as for values of the build_id type.
  //
  template <typename T>
  inline auto
  operator== (const T& x, const build_id& y)
    -> decltype (x.package == y.package               &&
                 x.configuration == y.configuration   &&
                 x.toolchain_name == y.toolchain_name &&
                 x.toolchain_version.epoch == y.toolchain_version.epoch)
  {
    return x.package == y.package               &&
           x.configuration == y.configuration   &&
           x.toolchain_name == y.toolchain_name &&
           compare_version_eq (x.toolchain_version, y.toolchain_version, true);
  }

  template <typename T>
  inline auto
  operator!= (const T& x, const build_id& y)
    -> decltype (x.package == y.package               &&
                 x.configuration == y.configuration   &&
                 x.toolchain_name == y.toolchain_name &&
                 x.toolchain_version.epoch == y.toolchain_version.epoch)
  {
    return x.package != y.package               ||
           x.configuration != y.configuration   ||
           x.toolchain_name != y.toolchain_name ||
           compare_version_ne (x.toolchain_version, y.toolchain_version, true);
  }

  // build_state
  //
  enum class build_state: std::uint8_t
  {
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

  // target_triplet
  //
  #pragma db map type(butl::target_triplet) as(string) \
    to((?).string ())                                  \
    from(butl::target_triplet (?))

  // operation_results
  //
  using bbot::operation_result;
  #pragma db value(operation_result) definition

  using bbot::operation_results;

  #pragma db object pointer(shared_ptr) session
  class build
  {
  public:
    using timestamp_type    = brep::timestamp;
    using package_name_type = brep::package_name;

    // Create the build object with the building state, non-existent status,
    // the timestamp set to now and the force state set to unforced.
    //
    build (string tenant,
           package_name_type,
           version,
           string configuration,
           string toolchain_name, version toolchain_version,
           optional<string> agent_fingerprint,
           optional<string> agent_challenge,
           string machine, string machine_summary,
           butl::target_triplet);

    build_id id;

    string& tenant;                     // Tracks id.package.tenant.
    package_name_type& package_name;    // Tracks id.package.name.
    upstream_version package_version;   // Original of id.package.version.
    string& configuration;              // Tracks id.configuration.
    string& toolchain_name;             // Tracks id.toolchain_name.
    upstream_version toolchain_version; // Original of id.toolchain_version.

    build_state state;

    // Time of the last state change (the creation time initially).
    //
    timestamp_type timestamp;

    force_state force;

    // Must be present for the built state, may be present for the building
    // state.
    //
    optional<result_status> status;

    // Time of setting the result status that can be considered as the build
    // task completion (currently all the result_status values). Initialized
    // with timestamp_nonexistent by default.
    //
    // Note that in the future we may not consider abort and abnormal as the
    // task completion and, for example, proceed with automatic rebuild (the
    // flake monitor idea).
    //
    timestamp_type completion_timestamp;

    // May be present only for the building state.
    //
    optional<string> agent_fingerprint;
    optional<string> agent_challenge;

    string machine;
    string machine_summary;
    butl::target_triplet target;

    // Note that the logs are stored as std::string/TEXT which is Ok since
    // they are UTF-8 and our database is UTF-8.
    //
    operation_results results;
    odb::section results_section;

    // Database mapping.
    //
    #pragma db member(id) id column("")

    #pragma db member(tenant) transient
    #pragma db member(package_name) transient
    #pragma db member(package_version) \
      set(this.package_version.init (this.id.package.version, (?)))
    #pragma db member(configuration) transient
    #pragma db member(toolchain_name) transient
    #pragma db member(toolchain_version) \
      set(this.toolchain_version.init (this.id.toolchain_version, (?)))

    // Speed-up queries with ordering the result by the timestamp.
    //
    #pragma db member(timestamp) index

    // @@ TMP remove when 0.13.0 is released.
    //
    #pragma db member(completion_timestamp) default(0)

    #pragma db member(results) id_column("") value_column("") \
      section(results_section)

    #pragma db member(results_section) load(lazy) update(always)

    build (const build&) = delete;
    build& operator= (const build&) = delete;

  private:
    friend class odb::access;

    build ()
        : tenant (id.package.tenant),
          package_name (id.package.name),
          configuration (id.configuration),
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

  // Build of an existing buildable package.
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
                 string configuration,
                 string toolchain_name, version toolchain_version,
                 timestamp package_timestamp);

    build_id id;

    string& tenant;                     // Tracks id.package.tenant.
    package_name_type& package_name;    // Tracks id.package.name.
    upstream_version package_version;   // Original of id.package.version.
    string& configuration;              // Tracks id.configuration.
    string& toolchain_name;             // Tracks id.toolchain_name.
    upstream_version toolchain_version; // Original of id.toolchain_version.

    // Time of the latest delay report. Initialized with timestamp_nonexistent
    // by default.
    //
    timestamp report_timestamp;

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
    #pragma db member(configuration) transient
    #pragma db member(toolchain_name) transient
    #pragma db member(toolchain_version) \
      set(this.toolchain_version.init (this.id.toolchain_version, (?)))

  private:
    friend class odb::access;

    build_delay ()
        : tenant (id.package.tenant),
          package_name (id.package.name),
          configuration (id.configuration),
          toolchain_name (id.toolchain_name) {}
  };
}

#endif // LIBBREP_BUILD_HXX
