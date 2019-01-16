// file      : libbrep/build.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
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
#define LIBBREP_BUILD_SCHEMA_VERSION_BASE 4

#pragma db model version(LIBBREP_BUILD_SCHEMA_VERSION_BASE, 6, open)

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
    canonical_version toolchain_version;

    build_id () = default;
    build_id (package_id p, string c, const brep::version& v)
        : package (move (p)),
          configuration (move (c)),
          toolchain_version {
            v.epoch, v.canonical_upstream, v.canonical_release, v.revision} {}
  };

  inline bool
  operator< (const build_id& x, const build_id& y)
  {
    if (x.package != y.package)
      return x.package < y.package;

    if (int r = x.configuration.compare (y.configuration))
      return r < 0;

    return compare_version_lt (x.toolchain_version, y.toolchain_version, true);
  }

  // These allow comparing objects that have package, configuration and
  // toolchain_version data members to build_id values. The idea is that this
  // works for both query members of build id types as well as for values of
  // the build_id type.
  //
  template <typename T>
  inline auto
  operator== (const T& x, const build_id& y)
    -> decltype (x.package == y.package             &&
                 x.configuration == y.configuration &&
                 x.toolchain_version.epoch == y.toolchain_version.epoch)
  {
    return x.package == y.package             &&
           x.configuration == y.configuration &&
           compare_version_eq (x.toolchain_version, y.toolchain_version, true);
  }

  template <typename T>
  inline auto
  operator!= (const T& x, const build_id& y)
    -> decltype (x.package == y.package             &&
                 x.configuration == y.configuration &&
                 x.toolchain_version.epoch == y.toolchain_version.epoch)
  {
    return x.package != y.package             ||
           x.configuration != y.configuration ||
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
    string toolchain_name;
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
    #pragma db member(toolchain_version) \
      set(this.toolchain_version.init (this.id.toolchain_version, (?)))

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
          configuration (id.configuration)
    {
    }
  };

  #pragma db view object(build)                                              \
    object(build_package inner:                                              \
           brep::operator== (build::id.package, build_package::id) &&        \
           build_package::internal_repository.canonical_name.is_not_null ()) \
    query(distinct)
  struct toolchain
  {
    string name;
    upstream_version version;

    // Database mapping. Note that the version member must be loaded after
    // the virtual members since the version_ member must filled by that time.
    //
    #pragma db member(name) column(build::toolchain_name)

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

  // Build of an existing internal package.
  //
  // Note that ADL can't find the equal operator, so we use the function call
  // notation.
  //
  #pragma db view                                                            \
    object(build)                                                            \
    object(build_package inner:                                              \
           brep::operator== (build::id.package, build_package::id) &&        \
           build_package::internal_repository.canonical_name.is_not_null ()) \
    object(build_tenant: build_package::id.tenant == build_tenant::id)
  struct package_build
  {
    shared_ptr<brep::build> build;
  };

  #pragma db view                                                            \
    object(build)                                                            \
    object(build_package inner:                                              \
           brep::operator== (build::id.package, build_package::id) &&        \
           build_package::internal_repository.canonical_name.is_not_null ()) \
    object(build_tenant: build_package::id.tenant == build_tenant::id)
  struct package_build_count
  {
    size_t result;

    operator size_t () const {return result;}

    // Database mapping.
    //
    #pragma db member(result) column("count(" + build::id.package.name + ")")
  };
}

#endif // LIBBREP_BUILD_HXX
