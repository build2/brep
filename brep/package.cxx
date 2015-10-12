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
      : id (move (nm), vr),
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
      : id (move (nm), vr),
        version (move (vr))
  {
    assert (!rp->internal);
    external_repositories.emplace_back (move (rp));
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
    name = location.canonical_name ();
  }

  repository::
  repository (repository_location l)
      : location (move (l)),
        internal (false)
  {
    name = location.canonical_name ();
  }
}
