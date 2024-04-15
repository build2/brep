// file      : libbrep/package.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBREP_PACKAGE_HXX
#define LIBBREP_PACKAGE_HXX

#include <map>
#include <chrono>

#include <odb/core.hxx>
#include <odb/section.hxx>
#include <odb/nested-container.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <libbrep/common.hxx> // Must be included last (see assert).

// Used by the data migration entries.
//
#define LIBBREP_PACKAGE_SCHEMA_VERSION_BASE 27

#pragma db model version(LIBBREP_PACKAGE_SCHEMA_VERSION_BASE, 33, closed)

namespace brep
{
  // @@ Might make sense to put some heavy members (e.g., description,
  //    containers) into a separate section.
  //
  // @@ Not sure there is a benefit in making topics/keywords full-blown
  //    containers (i.e., a separate table). Maybe provide a mapping of
  //    vector<string> to TEXT as a comma/space-separated list.
  //

  // Forward declarations.
  //
  class repository;
  class package;

  // priority
  //
  using bpkg::priority;

  #pragma db value(priority) definition
  #pragma db member(priority::value) column("")

  // text_type
  //
  using bpkg::text_type;
  using bpkg::to_text_type;

  // Note that here we assume that the saved string representation of a type
  // is always recognized later.
  //
  #pragma db map type(text_type) as(string) \
    to(to_string (?))                       \
    from(*brep::to_text_type (?))

  using optional_text_type = optional<text_type>;

  #pragma db map type(optional_text_type) as(brep::optional_string)     \
    to((?) ? to_string (*(?)) : brep::optional_string ())               \
    from((?) ? brep::to_text_type (*(?)) : brep::optional_text_type ())

  // manifest_url
  //
  using bpkg::manifest_url;

  #pragma db value(manifest_url) definition
  #pragma db member(manifest_url::value) virtual(string) before \
    get(this.string ())                                         \
    set(this = brep::manifest_url ((?), "" /* comment */))      \
    column("")

  // licenses
  //
  using bpkg::licenses;
  using license_alternatives = small_vector<licenses, 1>;

  #pragma db value(licenses) definition

  // dependencies
  //
  // Notes:
  //
  // 1. Will the package be always resolvable? What if it is in
  //    another repository (i.e., a "chained" third-party repo).
  //    The question is then whether we will load such "third-
  //    party packages" (i.e., packages that are not in our
  //    repository). If the answer is yes, then we can have
  //    a pointer here. If the answer is no, then we can't.
  //    Also, if the answer is yes, we probably don't need to
  //    load as much information as for "our own" packages. We
  //    also shouldn't be showing them in search results, etc.
  //    I think all we need is to know which repository this
  //    package comes from so that we can tell the user. How are
  //    we going to capture this? Poly hierarchy of packages?
  //
  // 2. I believe we don't need to use a weak pointer here since
  //    there should be no package dependency cycles (and therefore
  //    ownership cycles).
  //
  // 3. Actually there can be dependency cycle as dependency referes not to
  //    just a package but a specific version, so for the same pair of
  //    packages dependency for different versions can have an opposite
  //    directions. The possible solution is instead of a package we point
  //    to the earliest version that satisfies the constraint. But this
  //    approach requires to ensure no cycles exist before instantiating
  //    package objects which in presense of "foreign" packages can be
  //    tricky. Can stick to just a package name until get some clarity on
  //    "foreign" package resolution.
  //
  // 4. As we left just the package class the dependency resolution come to
  //    finding the best version matching package object. The question is
  //    if to resolve dependencies on the loading phase or in the WEB interface
  //    when required. The arguments in favour of doing that during loading
  //    phase are:
  //
  //    - WEB interface get offloaded from a possibly expensive queries
  //      which otherwise have to be executed multiple times for the same
  //      dependency no matter the result would be the same.
  //
  //    - No need to complicate persisted object model with repository
  //      relations otherwise required just for dependency resolution.
  //

  #pragma db value
  struct dependency
  {
    using package_type = brep::package;

    package_name name;
    optional<version_constraint> constraint;

    // Resolved dependency package. Can be NULL if the repository load was
    // shallow and the package dependency could not be resolved.
    //
    lazy_shared_ptr<package_type> package;

    // Database mapping.
    //
    #pragma db member(constraint) column("")
  };

  ostream&
  operator<< (ostream&, const dependency&);

  bool
  operator== (const dependency&, const dependency&);

  bool
  operator!= (const dependency&, const dependency&);

  #pragma db value
  class dependency_alternative: public small_vector<dependency, 1>
  {
  public:
    // While we currently don't use the reflect, prefer, accept, and require
    // values, let's save them for completeness.
    //
    optional<string> enable;
    optional<string> reflect;
    optional<string> prefer;
    optional<string> accept;
    optional<string> require;

    dependency_alternative () = default;
    dependency_alternative (optional<string> e,
                            optional<string> r,
                            optional<string> p,
                            optional<string> a,
                            optional<string> q)
        : enable (move (e)),
          reflect (move (r)),
          prefer (move (p)),
          accept (move (a)),
          require (move (q)) {}
  };

  #pragma db value
  class dependency_alternatives: public small_vector<dependency_alternative, 1>
  {
  public:
    bool buildtime;
    string comment;

    dependency_alternatives () = default;
    dependency_alternatives (bool b, string c)
        : buildtime (b), comment (move (c)) {}
  };

  using dependencies = vector<dependency_alternatives>;

  // tests
  //
  #pragma db value
  struct test_dependency: dependency
  {
    test_dependency_type type;
    bool buildtime;
    optional<string> enable;
    optional<string> reflect;

    test_dependency () = default;
    test_dependency (package_name n,
                     test_dependency_type t,
                     bool b,
                     optional<version_constraint> c,
                     optional<string> e,
                     optional<string> r)
        : dependency {move (n), move (c), nullptr /* package */},
          type (t),
          buildtime (b),
          enable (move (e)),
          reflect (move (r))
    {
    }

    // Database mapping.
    //
    #pragma db member(buildtime)
  };

  // certificate
  //
  #pragma db value
  class certificate
  {
  public:
    string fingerprint;  // SHA256 fingerprint. Note: foreign-mapped in build.
    string name;         // CN component of Subject.
    string organization; // O component of Subject.
    string email;        // email: in Subject Alternative Name.
    string pem;          // PEM representation.
  };

  #pragma db object pointer(shared_ptr) session
  class tenant
  {
  public:
    // Create the tenant object with the timestamp set to now and the archived
    // flag set to false.
    //
    tenant (string id,
            bool private_,
            optional<string> interactive,
            optional<tenant_service>);

    string id;

    // If this flag is true, then display the packages in the web interface
    // only in the tenant view mode.
    //
    bool private_;                        // Note: foreign-mapped in build.

    // Interactive package build breakpoint.
    //
    // If present, then packages from this tenant will only be built
    // interactively and only non-interactively otherwise.
    //
    optional<string> interactive;         // Note: foreign-mapped in build.

    timestamp creation_timestamp;
    bool archived = false;                // Note: foreign-mapped in build.

    optional<tenant_service> service;     // Note: foreign-mapped in build.

    // Note that due to the implementation complexity and performance
    // considerations, the service notifications are not synchronized. This
    // leads to a potential race, so that before we have sent the `queued`
    // notification for a package build, some other thread (potentially in a
    // different process) could have already sent the `building` notification
    // for it. It feels like there is no easy way to reliably fix that.
    // Instead, we just decrease the probability of such a notifications
    // sequence failure by delaying builds of the freshly queued packages for
    // some time.  Specifically, whenever the `queued` notification is ought
    // to be sent (normally out of the database transaction, since it likely
    // sends an HTTP request, etc) the tenant's queued_timestamp member is set
    // to the current time. During the configured time interval since that
    // time point the build tasks may not be issued for the tenant's packages.
    //
    // Also note that while there are similar potential races for other
    // notification sequences, their probability is rather low due to the
    // natural reasons (non-zero build task execution time, etc) and thus we
    // just ignore them.
    //
    optional<timestamp> queued_timestamp; // Note: foreign-mapped in build.

    // Note that after the package tenant is created but before the first
    // build object is created, there is no easy way to produce a list of
    // unbuilt package configurations. That would require to know the build
    // toolchain(s), which are normally extracted from the build objects.
    // Thus, the empty unbuilt package configurations list is ambiguous and
    // can either mean that no more package configurations can be built or
    // that we have not enough information to produce the list. To
    // disambiguate the empty list in the interface, in the latter case we
    // want to display the question mark instead of 0 as an unbuilt package
    // configurations count. To achieve this we will stash the build toolchain
    // in the tenant when a package from this tenant is considered for a build
    // for the first time but no configuration is picked for the build (the
    // target configurations are excluded, an auxiliary machine is not
    // available, etc). We will also use the stashed toolchain as a fallback
    // until we are able to retrieve the toolchain(s) from the tenant builds
    // to produce the unbuilt package configurations list.
    //
    // Note: foreign-mapped in build.
    //
    optional<brep::build_toolchain> build_toolchain;

    // Database mapping.
    //
    #pragma db member(id) id
    #pragma db member(private_)

    #pragma db index("tenant_service_i") \
      unique                             \
      members(service.id, service.type)

    #pragma db index member(service.id)

  private:
    friend class odb::access;
    tenant () = default;
  };

  #pragma db view object(tenant)
  struct tenant_id
  {
    #pragma db column("id")
    string value;
  };

  // Tweak repository_id mapping to include a constraint (this only affects
  // the database schema).
  //
  #pragma db member(repository_id::tenant) points_to(tenant)

  #pragma db object pointer(shared_ptr) session
  class repository
  {
  public:
    using email_type = brep::email;
    using certificate_type = brep::certificate;

    // Create internal repository.
    //
    repository (string tenant,
                repository_location,
                string display_name,
                repository_location cache_location,
                optional<certificate_type>,
                bool buildable,
                uint16_t priority);

    // Create external repository.
    //
    explicit
    repository (string tenant, repository_location);

    repository_id id;

    const string& tenant;         // Tracks id.tenant.
    const string& canonical_name; // Tracks id.canonical_name.
    repository_location location; // Note: foreign-mapped in build.
    string display_name;

    // The order in the internal repositories configuration file, starting
    // from 1. 0 for external repositories.
    //
    uint16_t priority;

    optional<string> interface_url;

    // Present only for internal repositories.
    //
    optional<email_type> email;
    optional<string> summary;
    optional<string> description;

    // Location of the repository local cache. Non empty for internal
    // repositories and external ones with a filesystem path location.
    //
    repository_location cache_location;

    // Present only for internal signed repositories. Note that it is
    // foreign-mapped in build.
    //
    optional<certificate_type> certificate;

    // Initialized with timestamp_nonexistent by default.
    //
    timestamp packages_timestamp;

    // Initialized with timestamp_nonexistent by default.
    //
    timestamp repositories_timestamp;

    bool internal;

    // Whether repository packages are buildable by the build bot controller
    // service. Can only be true for internal repositories.
    //
    bool buildable;

    vector<lazy_weak_ptr<repository>> complements;
    vector<lazy_weak_ptr<repository>> prerequisites;

    // Database mapping.
    //
    #pragma db member(id) id column("")

    #pragma db member(tenant) transient
    #pragma db member(canonical_name) transient

    #pragma db member(location)                                            \
      set(this.location = std::move (?);                                   \
          assert (this.canonical_name == this.location.canonical_name ()))

    #pragma db member(complements) id_column("repository_") \
      value_column("complement_") value_not_null

    #pragma db member(prerequisites) id_column("repository_") \
      value_column("prerequisite_") value_not_null

  private:
    friend class odb::access;
    repository (): tenant (id.tenant), canonical_name (id.canonical_name) {}
  };

  // The 'to' expression calls the PostgreSQL to_tsvector(weighted_text)
  // function overload (package-extra.sql). Since we are only interested
  // in "write-only" members of this type, make the 'from' expression
  // always return empty string (we still have to work the placeholder
  // in to keep overprotective ODB happy).
  //
  #pragma db map type("tsvector") as("TEXT")                       \
    to("to_tsvector((?)::weighted_text)") from("COALESCE('',(?))")

  // C++ type for weighted PostgreSQL tsvector.
  //
  #pragma db value type("tsvector")
  struct weighted_text
  {
    string a;
    string b;
    string c;
    string d;
  };

  #pragma db value
  struct typed_text
  {
    string text;
    text_type type;

    #pragma db member(text) column("")
  };

  // Tweak public_key_id mapping to include a constraint (this only affects the
  // database schema).
  //
  #pragma db member(public_key_id::tenant) points_to(tenant)

  #pragma db object pointer(shared_ptr) session
  class public_key: public string
  {
  public:
    public_key (string tenant, string fingerprint, string key)
        : string (move (key)), id (move (tenant), move (fingerprint)) {}

    public_key_id id;

    // Database mapping.
    //
    #pragma db member(id) id column("")

    #pragma db member(data) virtual(string) access(this)

  private:
    friend class odb::access;
    public_key () = default;
  };

  // package_build_config
  //
  using package_build_config =
    build_package_config_template<lazy_shared_ptr<public_key>>;

  using package_build_configs =
    build_package_configs_template<lazy_shared_ptr<public_key>>;

  #pragma db value(package_build_config) definition

  #pragma db member(package_build_config::builds) transient
  #pragma db member(package_build_config::constraints) transient
  #pragma db member(package_build_config::auxiliaries) transient
  #pragma db member(package_build_config::bot_keys) transient

  // package_build_bot_keys
  //
  using package_build_bot_keys = vector<lazy_shared_ptr<public_key>>;
  using package_build_bot_key_key = odb::nested_key<package_build_bot_keys>;

  using package_build_bot_keys_map = std::map<package_build_bot_key_key,
                                              lazy_shared_ptr<public_key>>;

  #pragma db value(package_build_bot_key_key)
  #pragma db member(package_build_bot_key_key::outer) column("config_index")
  #pragma db member(package_build_bot_key_key::inner) column("index")

  // Tweak package_id mapping to include a constraint (this only affects the
  // database schema).
  //
  #pragma db member(package_id::tenant) points_to(tenant)

  #pragma db object pointer(shared_ptr) session
  class package
  {
  public:
    using repository_type = brep::repository;
    using version_type = brep::version;
    using upstream_version_type = brep::upstream_version;
    using priority_type = brep::priority;
    using license_alternatives_type = brep::license_alternatives;
    using email_type = brep::email;
    using dependencies_type = brep::dependencies;
    using requirements_type = brep::requirements;
    using build_constraints_type = brep::build_constraints;
    using build_auxiliaries_type = brep::build_auxiliaries;

    // Create internal package object.
    //
    // Note: the default build package config is expected to always be present.
    //
    package (package_name,
             version_type,
             optional<string> upstream_version,
             package_name project,
             priority_type,
             string summary,
             license_alternatives_type,
             small_vector<string, 5> topics,
             small_vector<string, 5> keywords,
             optional<typed_text> description,
             optional<typed_text> package_description,
             optional<typed_text> changes,
             optional<manifest_url> url,
             optional<manifest_url> doc_url,
             optional<manifest_url> src_url,
             optional<manifest_url> package_url,
             optional<email_type>,
             optional<email_type> package_email,
             optional<email_type> build_email,
             optional<email_type> build_warning_email,
             optional<email_type> build_error_email,
             dependencies_type,
             requirements_type,
             small_vector<test_dependency, 1> tests,
             build_class_exprs,
             build_constraints_type,
             build_auxiliaries_type,
             package_build_bot_keys,
             package_build_configs,
             optional<path> location,
             optional<string> fragment,
             optional<string> sha256sum,
             shared_ptr<repository_type>);

    // Create external package object.
    //
    // External package can appear on the WEB interface only in dependency
    // list in the form of a link to the corresponding WEB page. The only
    // package information required to compose such a link is the package name,
    // version, and repository location.
    //
    // External package can also be a separate test for some primary package
    // (and belong to a complement but yet external repository), and so we may
    // need its build class expressions, constraints, and configurations to
    // decide if to build it together with the primary package or not (see
    // test-exclude task manifest value for details). Additionally, when the
    // test package is being built the auxiliary machines may also be
    // required.
    //
    // Note: the default build package config is expected to always be present.
    //
    package (package_name name,
             version_type,
             build_class_exprs,
             build_constraints_type,
             build_auxiliaries_type,
             package_build_configs,
             shared_ptr<repository_type>);

    bool
    internal () const noexcept {return internal_repository != nullptr;}

    bool
    stub () const noexcept
    {
      return version.compare (wildcard_version,
                              true /* ignore_revision */) == 0;
    }

    // Manifest data.
    //
    package_id id;

    const string& tenant;          // Tracks id.tenant.
    const package_name& name;      // Tracks id.name.
    upstream_version_type version;

    optional<string> upstream_version;

    // Matches the package name if the project name is not specified in
    // the manifest.
    //
    package_name project; // Note: foreign-mapped in build.

    priority_type priority;
    string summary;
    license_alternatives_type license_alternatives;
    small_vector<string, 5> topics;
    small_vector<string, 5> keywords;

    // Note that the descriptions and changes are absent if the respective
    // type is unknown.
    //
    optional<typed_text> description;
    optional<typed_text> package_description;
    optional<typed_text> changes;

    optional<manifest_url> url;
    optional<manifest_url> doc_url;
    optional<manifest_url> src_url;
    optional<manifest_url> package_url;
    optional<email_type> email;
    optional<email_type> package_email;
    optional<email_type> build_email;         // Note: foreign-mapped in build.
    optional<email_type> build_warning_email; // Note: foreign-mapped in build.
    optional<email_type> build_error_email;   // Note: foreign-mapped in build.
    dependencies_type dependencies;
    requirements_type requirements;           // Note: foreign-mapped in build.
    small_vector<test_dependency, 1> tests;   // Note: foreign-mapped in build.

    // Common build classes, constraints, auxiliaries, and bot keys that apply
    // to all configurations unless overridden.
    //
    build_class_exprs builds;                 // Note: foreign-mapped in build.
    build_constraints_type build_constraints; // Note: foreign-mapped in build.
    build_auxiliaries_type build_auxiliaries; // Note: foreign-mapped in build.
    package_build_bot_keys build_bot_keys;    // Note: foreign-mapped in build.
    package_build_configs build_configs;      // Note: foreign-mapped in build.

    // Group the build_configs, builds, and build_constraints members of this
    // object together with their respective nested configs entries into the
    // separate section for an explicit load.
    //
    // Note that while the build auxiliaries and bot keys are persisted via
    // the newly created package objects, they are only used via the
    // foreign-mapped build_package objects (see build-package.hxx for
    // details). Thus, we add them to the never-loaded unused_section (see
    // below).
    //
    odb::section build_section;

    // Note that it is foreign-mapped in build.
    //
    lazy_shared_ptr<repository_type> internal_repository;

    // Path to the package file. Present only for internal packages.
    //
    optional<path> location;

    // Present only for packages that come from the supporting fragmentation
    // internal repository (normally version control-based).
    //
    optional<string> fragment;

    // Present only for internal packages.
    //
    optional<string> sha256sum;

    vector<lazy_shared_ptr<repository_type>> other_repositories;

    // Whether the package is buildable by the build bot controller service
    // and the reason if it's not.
    //
    // While we could potentially calculate this flag on the fly, that would
    // complicate the database queries significantly.
    //
    bool buildable; // Note: foreign-mapped in build.
    optional<brep::unbuildable_reason> unbuildable_reason;

    // If this flag is true, then all the package configurations are buildable
    // with the custom build bots. If false, then all configurations are
    // buildable with the default bots. If nullopt, then some configurations
    // are buildable with the custom and some with the default build bots.
    //
    // Note: meaningless if buildable is false.
    //
    optional<bool> custom_bot; // Note: foreign-mapped in build.

  private:
    odb::section unused_section;

    // Database mapping.
    //
    #pragma db member(id) id column("")
    #pragma db member(tenant) transient
    #pragma db member(name) transient
    #pragma db member(version) set(this.version.init (this.id.version, (?)))

    // license
    //
    using _license_key = odb::nested_key<licenses>;
    using _licenses_type = std::map<_license_key, string>;

    #pragma db value(_license_key)
    #pragma db member(_license_key::outer) column("alternative_index")
    #pragma db member(_license_key::inner) column("index")

    #pragma db member(license_alternatives) id_column("") value_column("")
    #pragma db member(licenses)                                       \
      virtual(_licenses_type)                                         \
      after(license_alternatives)                                     \
      get(odb::nested_get (this.license_alternatives))                \
      set(odb::nested_set (this.license_alternatives, std::move (?))) \
      id_column("") key_column("") value_column("license")

    // topics
    //
    #pragma db member(topics) id_column("") value_column("topic")

    // keywords
    //
    #pragma db member(keywords) id_column("") value_column("keyword")

    // dependencies
    //
    // Note that this is a 2-level nested container which is mapped to three
    // container tables each containing data of each dimension.

    // Container of the dependency_alternatives values.
    //
    #pragma db member(dependencies) id_column("") value_column("")

    // Container of the dependency_alternative values.
    //
    using _dependency_alternative_key =
      odb::nested_key<dependency_alternatives>;

    using _dependency_alternatives_type =
      std::map<_dependency_alternative_key, dependency_alternative>;

    #pragma db value(_dependency_alternative_key)
    #pragma db member(_dependency_alternative_key::outer) column("dependency_index")
    #pragma db member(_dependency_alternative_key::inner) column("index")

    #pragma db member(dependency_alternatives)                \
      virtual(_dependency_alternatives_type)                  \
      after(dependencies)                                     \
      get(odb::nested_get (this.dependencies))                \
      set(odb::nested_set (this.dependencies, std::move (?))) \
      id_column("") key_column("") value_column("")

    // Container of the dependency values.
    //
    using _dependency_key = odb::nested2_key<dependency_alternatives>;
    using _dependency_alternative_dependencies_type =
      std::map<_dependency_key, dependency>;

    #pragma db value(_dependency_key)
    #pragma db member(_dependency_key::outer)  column("dependency_index")
    #pragma db member(_dependency_key::middle) column("alternative_index")
    #pragma db member(_dependency_key::inner)  column("index")

    #pragma db member(dependency_alternative_dependencies)     \
      virtual(_dependency_alternative_dependencies_type)       \
      after(dependency_alternatives)                           \
      get(odb::nested2_get (this.dependencies))                \
      set(odb::nested2_set (this.dependencies, std::move (?))) \
      id_column("") key_column("") value_column("dep_")

    // requirements
    //
    // Note that this is a 2-level nested container which is mapped to three
    // container tables each containing data of each dimension.

    // Container of the requirement_alternatives values.
    //
    #pragma db member(requirements) id_column("") value_column("")

    // Container of the requirement_alternative values.
    //
    #pragma db member(requirement_alternatives)               \
      virtual(requirement_alternatives_map)                   \
      after(requirements)                                     \
      get(odb::nested_get (this.requirements))                \
      set(odb::nested_set (this.requirements, std::move (?))) \
      id_column("") key_column("") value_column("")

    // Container of the requirement (string) values.
    //
    #pragma db member(requirement_alternative_requirements)    \
      virtual(requirement_alternative_requirements_map)        \
      after(requirement_alternatives)                          \
      get(odb::nested2_get (this.requirements))                \
      set(odb::nested2_set (this.requirements, std::move (?))) \
      id_column("") key_column("") value_column("id")

    // tests
    //
    #pragma db member(tests) id_column("") value_column("test_")

    // builds
    //
    #pragma db member(builds) id_column("") value_column("") \
      section(build_section)

    // build_constraints
    //
    #pragma db member(build_constraints) id_column("") value_column("") \
      section(build_section)

    // build_auxiliaries
    //
    #pragma db member(build_auxiliaries) id_column("") value_column("") \
      section(unused_section)

    // build_bot_keys
    //
    #pragma db member(build_bot_keys)                   \
      id_column("") value_column("key_") value_not_null \
      section(unused_section)

    // build_configs
    //
    // Note that package_build_config::{builds,constraints,auxiliaries,
    // bot_keys} are persisted/loaded via the separate nested containers (see
    // commons.hxx for details).
    //
    #pragma db member(build_configs) id_column("") value_column("config_") \
      section(build_section)

    #pragma db member(build_config_builds)                           \
      virtual(build_class_exprs_map)                                 \
      after(build_configs)                                           \
      get(odb::nested_get (                                          \
            brep::build_package_config_builds (this.build_configs))) \
      set(brep::build_package_config_builds bs;                      \
          odb::nested_set (bs, std::move (?));                       \
          move (bs).to_configs (this.build_configs))                 \
      id_column("") key_column("") value_column("")                  \
      section(build_section)

    #pragma db member(build_config_constraints)                           \
      virtual(build_constraints_map)                                      \
      after(build_config_builds)                                          \
      get(odb::nested_get (                                               \
            brep::build_package_config_constraints (this.build_configs))) \
      set(brep::build_package_config_constraints cs;                      \
          odb::nested_set (cs, std::move (?));                            \
          move (cs).to_configs (this.build_configs))                      \
      id_column("") key_column("") value_column("")                       \
      section(build_section)

    #pragma db member(build_config_auxiliaries)                           \
      virtual(build_auxiliaries_map)                                      \
      after(build_config_constraints)                                     \
      get(odb::nested_get (                                               \
            brep::build_package_config_auxiliaries (this.build_configs))) \
      set(brep::build_package_config_auxiliaries as;                      \
          odb::nested_set (as, std::move (?));                            \
          move (as).to_configs (this.build_configs))                      \
      id_column("") key_column("") value_column("")                       \
      section(unused_section)

    #pragma db member(build_config_bot_keys)                           \
      virtual(package_build_bot_keys_map)                              \
      after(build_config_auxiliaries)                                  \
      get(odb::nested_get (                                            \
            brep::build_package_config_bot_keys (this.build_configs))) \
      set(brep::build_package_config_bot_keys<                         \
            lazy_shared_ptr<brep::public_key>> bks;                    \
          odb::nested_set (bks, std::move (?));                        \
          move (bks).to_configs (this.build_configs))                  \
      id_column("") key_column("") value_column("key_") value_not_null \
      section(unused_section)

    #pragma db member(build_section)  load(lazy) update(always)
    #pragma db member(unused_section) load(lazy) update(manual)

    // other_repositories
    //
    #pragma db member(other_repositories)                     \
      id_column("") value_column("repository_") value_not_null

    // search_index
    //
    #pragma db member(search_index) virtual(weighted_text) null \
      access(search_text)

    #pragma db index method("GIN") member(search_index)

  private:
    friend class odb::access;
    package (): tenant (id.tenant), name (id.name) {}

    // Save keywords, summary, descriptions, and changes to weighted_text a,
    // b, c, d members, respectively. So a word found in keywords will have a
    // higher weight than if it's found in the summary.
    //
    weighted_text
    search_text () const;

    // Noop as search_index is a write-only member.
    //
    void
    search_text (const weighted_text&) {}
  };

  // Package search query matching rank.
  //
  #pragma db view query("/*CALL*/ SELECT * FROM search_latest_packages(?)")
  struct latest_package_search_rank
  {
    package_id id;
    double rank;
  };

  #pragma db view \
    query("/*CALL*/ SELECT count(*) FROM search_latest_packages(?)")
  struct latest_package_count
  {
    size_t result;

    operator size_t () const {return result;}
  };

  #pragma db view query("/*CALL*/ SELECT * FROM search_packages(?)")
  struct package_search_rank
  {
    package_id id;
    double rank;
  };

  #pragma db view query("/*CALL*/ SELECT count(*) FROM search_packages(?)")
  struct package_count
  {
    size_t result;

    operator size_t () const {return result;}
  };

  #pragma db view query("/*CALL*/ SELECT * FROM latest_package(?)")
  struct latest_package
  {
    package_id id;
  };
}

// Workaround for GCC __is_invocable/non-constant condition bug (#86441).
//
#ifdef ODB_COMPILER
namespace std
{
  template class map<brep::package::_license_key, string>;
}
#endif

#endif // LIBBREP_PACKAGE_HXX
