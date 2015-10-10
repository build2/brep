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
  // package
  //
  package::
  package (string nm,
           version_type vr,
           priority_type pr,
           string sm,
           license_alternatives_type la,
           strings tg,
           optional<string> ds,
           string ch,
           url_type ur,
           optional<url_type> pu,
           email_type em,
           optional<email_type> pe,
           dependencies_type dp,
           requirements_type rq,
           optional<path> lc,
           shared_ptr<repository_type> rp)
      : name (move (nm)),
        version (move (vr)),
        priority (move (pr)),
        summary (move (sm)),
        license_alternatives (move (la)),
        tags (move (tg)),
        description (move (ds)),
        changes (move (ch)),
        url (move (ur)),
        package_url (move (pu)),
        email (move (em)),
        package_email (move (pe)),
        dependencies (move (dp)),
        requirements (move (rq)),
        internal_repository (move (rp)),
        location (move (lc))
  {
    assert (internal_repository->internal);
  }

  package::
  package (string nm,
           version_type vr,
           shared_ptr<repository_type> rp)
      : name (move (nm)),
        version (move (vr))
  {
    assert (!rp->internal);
    external_repositories.emplace_back (move (rp));
  }

  package::_id_type package::
  _id () const
  {
    return _id_type {
      {
        name,
        version.epoch,
        version.canonical_upstream,
        version.revision
      },
      version.upstream};
  }

  void package::
  _id (_id_type&& v, database&)
  {
    const auto& dv (v.data.version);
    name = move (v.data.name);
    version = version_type (dv.epoch, move (v.upstream), dv.revision);
    assert (version.canonical_upstream == dv.canonical_upstream);
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
