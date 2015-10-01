// file      : brep/package.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/package>

#include <utility> // move()
#include <cassert>

#include <odb/database.hxx>

#include <brep/package-odb>

using namespace std;
using namespace odb::core;

namespace brep
{
  // Utility functions
  //
  static inline bool
  alpha (char c)
  {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  }

  static inline bool
  digit (char c)
  {
    return c >= '0' && c <= '9';
  }

  // package
  //
  package::
  package (string n,
           string s,
           strings t,
           optional<string> d,
           url_type u,
           optional<url_type> pu,
           email_type e,
           optional<email_type> pe)
      : name (move (n)),
        summary (move (s)),
        tags (move (t)),
        description (move (d)),
        url (move (u)),
        package_url (move (pu)),
        email (move (e)),
        package_email (move (pe))
  {
  }

  // package_version
  //
  package_version::
  package_version (lazy_shared_ptr<package_type> pk,
                   version_type vr,
                   priority_type pr,
                   license_alternatives_type la,
                   string ch,
                   dependencies_type dp,
                   requirements_type rq,
                   optional<path> lc,
                   shared_ptr<repository_type> rp)
      : package (move (pk)),
        version (move (vr)),
        priority (move (pr)),
        license_alternatives (move (la)),
        changes (move (ch)),
        dependencies (move (dp)),
        requirements (move (rq)),
        location (move (lc))
  {
    //@@ Can't be sure we are in transaction. Instead, make caller
    //   pass shared_ptr.
    //
    if (rp->internal)
      internal_repository = move (rp);
    else
      external_repositories.emplace_back (move (rp));
  }

  package_version::_id_type package_version::
  _id () const
  {
    return _id_type {
      {
        package.object_id (),
        version.epoch,
        version.canonical_upstream,
        version.revision
      },
      version.upstream};
  }

  void package_version::
  _id (_id_type&& v, database& db)
  {
    package = lazy_shared_ptr<package_type> (db, v.data.package);
    version = version_type (v.data.epoch, move (v.upstream), v.data.revision);
    assert (version.canonical_upstream == v.data.canonical_upstream);
  }

  // max_package_version
  //
  void max_package_version::
  _id (package_version::_id_type&& v)
  {
    version = version_type (v.data.epoch, move (v.upstream), v.data.revision);
    assert (version.canonical_upstream == v.data.canonical_upstream);
  }

  // repository
  //
  repository::
  repository (repository_location l, string d, dir_path p)
      : location (move (l)),
        display_name (move (d)),
        local_path (move (p)),
        internal (true)
  {
  }

  repository::_id_type repository::
  _id () const
  {
    return _id_type {location.canonical_name (), location.string ()};
  }

  void repository::
  _id (_id_type&& l)
  {
    location = repository_location (move (l.location));
    assert (location.canonical_name () == l.canonical_name);
  }
}
