// file      : libbrep/build-package.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBREP_BUILD_PACKAGE_HXX
#define LIBBREP_BUILD_PACKAGE_HXX

#include <odb/core.hxx>

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
  // Foreign object that is mapped to a subset of repository object.
  //
  #pragma db object table("build_repository") pointer(shared_ptr) readonly
  class build_repository
  {
  public:
    string name; // Object id (canonical name).
    repository_location location;
    optional<string> certificate_fingerprint;

    // Database mapping.
    //
    #pragma db member(name) id

    #pragma db member(location)                                  \
      set(this.location = std::move (?);                         \
          assert (this.name == this.location.canonical_name ()))

  private:
    friend class odb::access;
    build_repository () = default;
  };

  // "Foreign" value type that is mapped to a subset of the build_constraint
  // value type (see libbpkg/manifest.hxx for details).
  //
  #pragma db value
  struct build_constraint_subset
  {
    bool exclusion;
    string config;
    optional<string> target;
  };

  // Foreign object that is mapped to a subset of package object.
  //
  #pragma db object table("build_package") pointer(shared_ptr) readonly
  class build_package
  {
  public:
    package_id id;
    upstream_version version;
    lazy_shared_ptr<build_repository> internal_repository;

    // Mapped to a subset of the package object build_constraints member
    // using the PostgreSQL foreign table mechanism.
    //
    vector<build_constraint_subset> constraints;

    // Database mapping.
    //
    #pragma db member(id) id column("")
    #pragma db member(version) set(this.version.init (this.id.version, (?)))
    #pragma db member(constraints) id_column("") value_column("")

  private:
    friend class odb::access;
    build_package () = default;
  };

  // Packages that can potentially be built (internal non-stub).
  //
  #pragma db view                                                          \
    object(build_package)                                                  \
    object(build_repository inner:                                         \
           build_package::internal_repository == build_repository::name && \
           brep::compare_version_ne (build_package::id.version,            \
                                     brep::wildcard_version,               \
                                     false))
  struct buildable_package
  {
    package_id id;
    upstream_version version;

    // Database mapping.
    //
    #pragma db member(version) set(this.version.init (this.id.version, (?)))
  };

  #pragma db view                                                          \
    object(build_package)                                                  \
    object(build_repository inner:                                         \
           build_package::internal_repository == build_repository::name && \
           brep::compare_version_ne (build_package::id.version,            \
                                     brep::wildcard_version,               \
                                     false))
  struct buildable_package_count
  {
    size_t result;

    operator size_t () const {return result;}

    // Database mapping.
    //
    #pragma db member(result) column("count(" + build_package::id.name + ")")
  };

  // Packages that have the build constraints. Note that only buildable
  // (internal and non-stub) packages can have such constraints, so there is
  // no need for additional checks.
  //
  #pragma db view                                                     \
    table("build_package_constraints" = "c")                          \
    object(build_package = package inner:                             \
           "c.exclusion AND "                                         \
           "c.name = " + package::id.name + "AND" +                   \
           "c.version_epoch = " + package::id.version.epoch + "AND" + \
           "c.version_canonical_upstream = " +                        \
             package::id.version.canonical_upstream + "AND" +         \
           "c.version_canonical_release = " +                         \
             package::id.version.canonical_release + "AND" +          \
           "c.version_revision = " + package::id.version.revision)    \
    query(distinct)
  struct build_constrained_package
  {
    shared_ptr<build_package> package;
  };
}

#endif // LIBBREP_BUILD_PACKAGE_HXX
