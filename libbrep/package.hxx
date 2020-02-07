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
#define LIBBREP_PACKAGE_SCHEMA_VERSION_BASE 17

#pragma db model version(LIBBREP_PACKAGE_SCHEMA_VERSION_BASE, 17, closed)

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

  #pragma db map type(text_type) as(string) \
    to(to_string (?))                       \
    from(brep::to_text_type (?))

  using optional_text_type = optional<text_type>;

  #pragma db map type(optional_text_type) as(brep::optional_string)     \
    to((?) ? to_string (*(?)) : brep::optional_string ())               \
    from((?) ? brep::to_text_type (*(?)) : brep::optional_text_type ())

  // url
  //
  using bpkg::url;

  #pragma db value(url) definition
  #pragma db member(url::value) virtual(string) before \
    get(this.string ())                                \
    set(this = brep::url ((?), "" /* comment */))      \
    column("")

  // email
  //
  using bpkg::email;

  #pragma db value(email) definition
  #pragma db member(email::value) virtual(string) before access(this) column("")

  // licenses
  //
  using bpkg::licenses;
  using license_alternatives = vector<licenses>;

  #pragma db value(licenses) definition

  // dependencies
  //
  using bpkg::version_constraint;

  #pragma db value(version_constraint) definition

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

    // Resolved dependency package. NULL if the repository load was shallow
    // and so the package dependencies are not resolved.
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
  class dependency_alternatives: public vector<dependency>
  {
  public:
    bool conditional;
    bool buildtime;
    string comment;

    dependency_alternatives () = default;
    dependency_alternatives (bool d, bool b, string c)
        : conditional (d), buildtime (b), comment (move (c)) {}
  };

  using dependencies = vector<dependency_alternatives>;

  // requirements
  //
  using bpkg::requirement_alternatives;
  using requirements = vector<requirement_alternatives>;

  #pragma db value(requirement_alternatives) definition

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
    explicit
    tenant (string id);

    string id;

    timestamp creation_timestamp;
    bool archived = false;        // Note: foreign-mapped in build.

    // Database mapping.
    //
    #pragma db member(id) id

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
    using url_type = brep::url;
    using email_type = brep::email;
    using dependencies_type = brep::dependencies;
    using requirements_type = brep::requirements;
    using build_constraints_type = brep::build_constraints;

    // Create internal package object. Note that for stubs the build
    // constraints are meaningless, and so not saved.
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
             optional<string> description,
             optional<text_type> description_type,
             string changes,
             optional<url_type>,
             optional<url_type> doc_url,
             optional<url_type> src_url,
             optional<url_type> package_url,
             optional<email_type>,
             optional<email_type> package_email,
             optional<email_type> build_email,
             optional<email_type> build_warning_email,
             optional<email_type> build_error_email,
             dependencies_type,
             requirements_type,
             small_vector<dependency, 1> tests,
             small_vector<dependency, 1> examples,
             small_vector<dependency, 1> benchmarks,
             build_class_exprs,
             build_constraints_type,
             optional<path> location,
             optional<string> fragment,
             optional<string> sha256sum,
             shared_ptr<repository_type>);

    // Create external package object.
    //
    // External repository packages can appear on the WEB interface only in
    // dependency list in the form of a link to the corresponding WEB page.
    // The only package information required to compose such a link is the
    // package name, version, and repository location.
    //
    package (package_name name, version_type, shared_ptr<repository_type>);

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
    package_name project;

    priority_type priority;
    string summary;
    license_alternatives_type license_alternatives;
    small_vector<string, 5> topics;
    small_vector<string, 5> keywords;
    optional<string> description;         // Absent if type is unknown.
    optional<text_type> description_type; // Present if description is present.
    string changes;
    optional<url_type> url;
    optional<url_type> doc_url;
    optional<url_type> src_url;
    optional<url_type> package_url;
    optional<email_type> email;
    optional<email_type> package_email;
    optional<email_type> build_email;
    optional<email_type> build_warning_email;
    optional<email_type> build_error_email;
    dependencies_type dependencies;
    requirements_type requirements;
    small_vector<dependency, 1> tests;
    small_vector<dependency, 1> examples;
    small_vector<dependency, 1> benchmarks;

    build_class_exprs builds;                 // Note: foreign-mapped in build.
    build_constraints_type build_constraints; // Note: foreign-mapped in build.
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

    // Whether the package is buildable by the build bot controller service.
    // Can only be true for non-stubs that belong to at least one buildable
    // (internal) repository.
    //
    // While we could potentially calculate this flag on the fly, that would
    // complicate the database queries significantly.
    //
    // Note: foreign-mapped in build.
    //
    bool buildable;

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
    using _dependency_key = odb::nested_key<dependency_alternatives>;
    using _dependency_alternatives_type =
               std::map<_dependency_key, dependency>;

    #pragma db value(_dependency_key)
    #pragma db member(_dependency_key::outer) column("dependency_index")
    #pragma db member(_dependency_key::inner) column("index")

    #pragma db member(dependencies) id_column("") value_column("")
    #pragma db member(dependency_alternatives)                \
      virtual(_dependency_alternatives_type)                  \
      after(dependencies)                                     \
      get(odb::nested_get (this.dependencies))                \
      set(odb::nested_set (this.dependencies, std::move (?))) \
      id_column("") key_column("") value_column("dep_")

    // requirements
    //
    using _requirement_key = odb::nested_key<requirement_alternatives>;
    using _requirement_alternatives_type =
               std::map<_requirement_key, string>;

    #pragma db value(_requirement_key)
    #pragma db member(_requirement_key::outer) column("requirement_index")
    #pragma db member(_requirement_key::inner) column("index")

    #pragma db member(requirements) id_column("") value_column("")
    #pragma db member(requirement_alternatives)               \
      virtual(_requirement_alternatives_type)                 \
      after(requirements)                                     \
      get(odb::nested_get (this.requirements))                \
      set(odb::nested_set (this.requirements, std::move (?))) \
      id_column("") key_column("") value_column("id")

    // tests, examples, benchmarks
    //
    // Seeing that these reuse the dependency types, we are also going to
    // have identical database mapping.
    //
    #pragma db member(tests) id_column("") value_column("dep_")
    #pragma db member(examples) id_column("") value_column("dep_")
    #pragma db member(benchmarks) id_column("") value_column("dep_")

    // builds
    //
    #pragma db member(builds) id_column("") value_column("") \
      section(build_section)

    // build_constraints
    //
    #pragma db member(build_constraints) id_column("") value_column("") \
      section(build_section)

    #pragma db member(build_section) load(lazy) update(always)

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

    // Save keywords, summary, description, and changes to weighted_text
    // a, b, c, d members, respectively. So a word found in keywords will
    // have a higher weight than if it's found in the summary.
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
