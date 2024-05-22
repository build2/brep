// file      : libbrep/build-package.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBREP_BUILD_PACKAGE_HXX
#define LIBBREP_BUILD_PACKAGE_HXX

#include <odb/core.hxx>
#include <odb/section.hxx>
#include <odb/nested-container.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <libbrep/common.hxx> // Must be included last (see assert).

namespace brep
{
  // These are "foreign objects" that are mapped to subsets of the package
  // database objects using the PostgreSQL foreign table mechanism. Note that
  // since we maintain the pair in sync by hand, we should only have a minimal
  // subset of "core" members (ideally just the primary key) that are unlikly
  // to disappear or change.
  //
  // The mapping is established in build-extra.sql. We also explicitly mark
  // non-primary key foreign-mapped members in the source object.

  // Foreign object that is mapped to a subset of the tenant object.
  //
  // Note: table created manually thus assign table name explicitly.
  //
  #pragma db object table("build_tenant") pointer(shared_ptr)
  class build_tenant
  {
  public:
    // Create tenant for an unloaded CI request (see the build_unloaded()
    // tenant services notification for details).
    //
    build_tenant (string i, tenant_service s, timestamp t, duration n)
        : id (move (i)),
          creation_timestamp (timestamp::clock::now ()),
          service (move (s)),
          unloaded_timestamp (t),
          unloaded_notify_interval (n) {}

    string id;

    bool private_ = false;
    optional<string> interactive;
    timestamp creation_timestamp;
    bool archived = false;
    optional<tenant_service> service;
    optional<timestamp> unloaded_timestamp;
    optional<duration> unloaded_notify_interval;
    optional<timestamp> queued_timestamp;
    optional<build_toolchain> toolchain;

    // Database mapping.
    //
    #pragma db member(id) id

  private:
    friend class odb::access;
    build_tenant () = default;
  };

  // Foreign object that is mapped to a subset of the repository object.
  //
  // Note: table created manually thus assign table name explicitly.
  //
  #pragma db object table("build_repository") pointer(shared_ptr) readonly
  class build_repository
  {
  public:
    repository_id id;

    const string& canonical_name;             // Tracks id.canonical_name.
    repository_location location;
    optional<string> certificate_fingerprint;

    // Database mapping.
    //
    #pragma db member(id) id column("")

    #pragma db member(canonical_name) transient

    #pragma db member(location)                                            \
      set(this.location = std::move (?);                                   \
          assert (this.canonical_name == this.location.canonical_name ()))

  private:
    friend class odb::access;
    build_repository (): canonical_name (id.canonical_name) {}
  };

  // Foreign object that is mapped to a subset of the public key object.
  //
  // Note: table created manually thus assign table name explicitly.
  //
  #pragma db object table("build_public_key") pointer(shared_ptr) readonly
  class build_public_key: public string
  {
  public:
    public_key_id id;

    // Database mapping.
    //
    #pragma db member(id) id column("")

    #pragma db member(data) virtual(string) access(this)

  private:
    friend class odb::access;
    build_public_key () = default;
  };

  // build_package_config
  //
  using build_package_config =
    build_package_config_template<lazy_shared_ptr<build_public_key>>;

  using build_package_configs =
    build_package_configs_template<lazy_shared_ptr<build_public_key>>;

  #pragma db value(build_package_config) definition

  #pragma db member(build_package_config::builds) transient
  #pragma db member(build_package_config::constraints) transient
  #pragma db member(build_package_config::auxiliaries) transient
  #pragma db member(build_package_config::bot_keys) transient

  // build_package_bot_keys
  //
  using build_package_bot_keys = vector<lazy_shared_ptr<build_public_key>>;
  using build_package_bot_key_key = odb::nested_key<build_package_bot_keys>;

  using build_package_bot_keys_map =
    std::map<build_package_bot_key_key, lazy_shared_ptr<build_public_key>>;

  #pragma db value(build_package_bot_key_key)
  #pragma db member(build_package_bot_key_key::outer) column("config_index")
  #pragma db member(build_package_bot_key_key::inner) column("index")

  // Forward declarations.
  //
  class build_package;

  // Build package dependency.
  //
  #pragma db value
  struct build_dependency
  {
    package_name name;
    optional<version_constraint> constraint;

    lazy_shared_ptr<build_package> package;

    // Database mapping.
    //
    #pragma db member(constraint) column("")
  };

  // Build package external test dependency.
  //
  #pragma db value
  struct build_test_dependency: build_dependency
  {
    test_dependency_type type;
    bool buildtime;
    optional<string> enable;
    optional<string> reflect;
  };

  // Foreign object that is mapped to a subset of the package object.
  //
  // Note: table created manually thus assign table name explicitly.
  //
  #pragma db object table("build_package") pointer(shared_ptr) readonly session
  class build_package
  {
  public:
    using requirements_type = brep::requirements;

    package_id id;
    upstream_version version;

    package_name project;

    optional<email> build_email;
    optional<email> build_warning_email;
    optional<email> build_error_email;

    // Mapped to the package object requirements and tests members using the
    // PostgreSQL foreign table mechanism.
    //
    requirements_type requirements;
    small_vector<build_test_dependency, 1> tests;

    odb::section requirements_tests_section;

    lazy_shared_ptr<build_repository> internal_repository;
    bool buildable;
    optional<bool> custom_bot;

    // Mapped to the package object builds, build_constraints,
    // build_auxiliaries, bot_keys, and build_configs members using the
    // PostgreSQL foreign table mechanism.
    //
    build_class_exprs      builds;
    build_constraints      constraints;
    build_auxiliaries      auxiliaries;
    build_package_bot_keys bot_keys;
    build_package_configs  configs;

    // Group the builds/constraints, auxiliaries, and bot_keys members of this
    // object together with their respective nested configs entries into the
    // separate sections for an explicit load. Note that the configs top-level
    // members are loaded implicitly.
    //
    odb::section constraints_section;
    odb::section auxiliaries_section;
    odb::section bot_keys_section;

    bool
    internal () const noexcept {return internal_repository != nullptr;}

    // Database mapping.
    //
    #pragma db member(id) id column("")
    #pragma db member(version) set(this.version.init (this.id.version, (?)))

    // requirements
    //
    // Note that this is a 2-level nested container (see package.hxx for
    // details).
    //

    // Container of the requirement_alternatives values.
    //
    #pragma db member(requirements) id_column("") value_column("") \
      section(requirements_tests_section)

    // Container of the requirement_alternative values.
    //
    #pragma db member(requirement_alternatives)               \
      virtual(requirement_alternatives_map)                   \
      after(requirements)                                     \
      get(odb::nested_get (this.requirements))                \
      set(odb::nested_set (this.requirements, std::move (?))) \
      id_column("") key_column("") value_column("")           \
      section(requirements_tests_section)

    // Container of the requirement (string) values.
    //
    #pragma db member(requirement_alternative_requirements)    \
      virtual(requirement_alternative_requirements_map)        \
      after(requirement_alternatives)                          \
      get(odb::nested2_get (this.requirements))                \
      set(odb::nested2_set (this.requirements, std::move (?))) \
      id_column("") key_column("") value_column("id")          \
      section(requirements_tests_section)

    // tests
    //
    #pragma db member(tests) id_column("") value_column("test_") \
      section(requirements_tests_section)

    #pragma db member(requirements_tests_section) load(lazy) update(always)

    // builds, constraints, auxiliaries, and bot_keys
    //
    #pragma db member(builds) id_column("") value_column("") \
      section(constraints_section)

    #pragma db member(constraints) id_column("") value_column("") \
      section(constraints_section)

    #pragma db member(auxiliaries) id_column("") value_column("") \
      section(auxiliaries_section)

    #pragma db member(bot_keys) id_column("") value_column("key_") \
      section(bot_keys_section)

    // configs
    //
    // Note that build_package_config::{builds,constraints,auxiliaries,bot_keys}
    // are persisted/loaded via the separate nested containers (see
    // commons.hxx for details).
    //
    #pragma db member(configs) id_column("") value_column("config_")

    #pragma db member(config_builds)                           \
      virtual(build_class_exprs_map)                           \
      after(configs)                                           \
      get(odb::nested_get (                                    \
            brep::build_package_config_builds (this.configs))) \
      set(brep::build_package_config_builds bs;                \
          odb::nested_set (bs, std::move (?));                 \
          move (bs).to_configs (this.configs))                 \
      id_column("") key_column("") value_column("")            \
      section(constraints_section)

    #pragma db member(config_constraints)                           \
      virtual(build_constraints_map)                                \
      after(config_builds)                                          \
      get(odb::nested_get (                                         \
            brep::build_package_config_constraints (this.configs))) \
      set(brep::build_package_config_constraints cs;                \
          odb::nested_set (cs, std::move (?));                      \
          move (cs).to_configs (this.configs))                      \
      id_column("") key_column("") value_column("")                 \
      section(constraints_section)

    #pragma db member(config_auxiliaries)                           \
      virtual(build_auxiliaries_map)                                \
      after(config_constraints)                                     \
      get(odb::nested_get (                                         \
            brep::build_package_config_auxiliaries (this.configs))) \
      set(brep::build_package_config_auxiliaries as;                \
          odb::nested_set (as, std::move (?));                      \
          move (as).to_configs (this.configs))                      \
      id_column("") key_column("") value_column("")                 \
      section(auxiliaries_section)

    #pragma db member(config_bot_keys)                                  \
      virtual(build_package_bot_keys_map)                               \
      after(config_auxiliaries)                                         \
      get(odb::nested_get (                                             \
            brep::build_package_config_bot_keys<                        \
              lazy_shared_ptr<brep::build_public_key>> (this.configs))) \
      set(brep::build_package_config_bot_keys<                          \
            lazy_shared_ptr<brep::build_public_key>> bks;               \
          odb::nested_set (bks, std::move (?));                         \
          move (bks).to_configs (this.configs))                         \
      id_column("") key_column("") value_column("key_")                 \
      section(bot_keys_section)

    #pragma db member(constraints_section) load(lazy) update(always)
    #pragma db member(auxiliaries_section) load(lazy) update(always)
    #pragma db member(bot_keys_section)    load(lazy) update(always)

  private:
    friend class odb::access;
    build_package () = default;
  };

  #pragma db view object(build_package)
  struct build_package_version
  {
    package_id id;
    upstream_version version;

    // Database mapping.
    //
    #pragma db member(version) set(this.version.init (this.id.version, (?)))
  };

  // Packages that can potentially be built.
  //
  // Note that ADL can't find the equal operator, so we use the function call
  // notation.
  //
  #pragma db view                                                      \
    object(build_package)                                              \
    object(build_repository inner:                                     \
           build_package::buildable &&                                 \
           brep::operator== (build_package::internal_repository,       \
                             build_repository::id))                    \
    object(build_tenant: build_package::id.tenant == build_tenant::id)
  struct buildable_package
  {
    shared_ptr<build_package> package;

    bool archived; // True if the tenant the package belongs to is archived.

    // Present if the tenant the package belongs to is interactive.
    //
    optional<string> interactive;
  };

  #pragma db view                                                      \
    object(build_package)                                              \
    object(build_repository inner:                                     \
           build_package::buildable &&                                 \
           brep::operator== (build_package::internal_repository,       \
                             build_repository::id))                    \
    object(build_tenant: build_package::id.tenant == build_tenant::id)
  struct buildable_package_count
  {
    size_t result;

    operator size_t () const {return result;}

    // Database mapping.
    //
    #pragma db member(result) column("count(" + build_package::id.name + ")")
  };
}

#endif // LIBBREP_BUILD_PACKAGE_HXX
