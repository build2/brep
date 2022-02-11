// file      : libbrep/build-package.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBREP_BUILD_PACKAGE_HXX
#define LIBBREP_BUILD_PACKAGE_HXX

#include <odb/core.hxx>
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
  //
  // Foreign object that is mapped to a subset of the tenant object.
  //
  #pragma db object table("build_tenant") pointer(shared_ptr) readonly
  class build_tenant
  {
  public:
    string id;

    bool private_;
    optional<string> interactive;
    bool archived;

    // Database mapping.
    //
    #pragma db member(id) id

  private:
    friend class odb::access;
    build_tenant () = default;
  };

  // Foreign object that is mapped to a subset of the repository object.
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
    optional<string> reflect;
  };

  // Foreign object that is mapped to a subset of the package object.
  //
  #pragma db object table("build_package") pointer(shared_ptr) readonly session
  class build_package
  {
  public:
    using requirements_type = brep::requirements;

    package_id id;
    upstream_version version;

    // Mapped to the package object requirements and tests members using the
    // PostgreSQL foreign table mechanism.
    //
    requirements_type requirements;
    small_vector<build_test_dependency, 1> tests;

    lazy_shared_ptr<build_repository> internal_repository;
    bool buildable;

    // Mapped to the package object builds and build_constraints members using
    // the PostgreSQL foreign table mechanism.
    //
    build_class_exprs builds;
    build_constraints constraints;

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
    #pragma db member(requirements) id_column("") value_column("")

    // Container of the requirement_alternative values.
    //
    #pragma db member(requirement_alternative_key::outer) column("requirement_index")
    #pragma db member(requirement_alternative_key::inner) column("index")

    #pragma db member(requirement_alternatives)               \
      virtual(requirement_alternatives_map)                   \
      after(requirements)                                     \
      get(odb::nested_get (this.requirements))                \
      set(odb::nested_set (this.requirements, std::move (?))) \
      id_column("") key_column("") value_column("")

    // Container of the requirement (string) values.
    //
    #pragma db member(requirement_key::outer)  column("requirement_index")
    #pragma db member(requirement_key::middle) column("alternative_index")
    #pragma db member(requirement_key::inner)  column("index")

    #pragma db member(requirement_alternative_requirements)    \
      virtual(requirement_alternative_requirements_map)        \
      after(requirement_alternatives)                          \
      get(odb::nested2_get (this.requirements))                \
      set(odb::nested2_set (this.requirements, std::move (?))) \
      id_column("") key_column("") value_column("id")

    // tests, builds, and constraints
    //
    #pragma db member(tests) id_column("") value_column("test_")
    #pragma db member(builds) id_column("") value_column("")
    #pragma db member(constraints) id_column("") value_column("")

  private:
    friend class odb::access;
    build_package () = default;
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
    package_id id;
    upstream_version version;

    bool archived; // True if the tenant the package belongs to is archived.

    // Database mapping.
    //
    #pragma db member(version) set(this.version.init (this.id.version, (?)))
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
