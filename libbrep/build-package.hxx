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
  // This is a "foreign object" that is mapped to the subset of package object
  // using PostgreSQL foreign table mechanism. Note that since we maintain the
  // two in sync by hand, we should only a have a minimal subset of "core"
  // members (ideally just the primary key) that are unlikly to disappear or
  // change.
  //
  // The mapping is established in build-extra.sql.
  //
  #pragma db object table("build_package") pointer(shared_ptr) readonly
  class build_package
  {
  public:
    package_id id;
    upstream_version version;
    optional<string> internal_repository;

    // Database mapping.
    //
    #pragma db member(id) id column("")
    #pragma db member(version) set(this.version.init (this.id.version, (?)))

  private:
    friend class odb::access;
    build_package () = default;
  };

  #pragma db view object(build_package)
  struct build_package_count
  {
    size_t result;

    operator size_t () const {return result;}

    // Database mapping.
    //
    #pragma db member(result) column("count(" + build_package::id.name + ")")
  };
}

#endif // LIBBREP_BUILD_PACKAGE_HXX
