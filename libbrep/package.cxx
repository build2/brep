// file      : libbrep/package.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbrep/package.hxx>

#include <odb/database.hxx>

#include <libbrep/package-odb.hxx>

using namespace std;
using namespace odb::core;

namespace brep
{
  // dependency
  //
  string dependency::
  name () const
  {
    return package.object_id ().name;
  }

  ostream&
  operator<< (ostream& o, const dependency& d)
  {
    o << d.name ();

    if (d.constraint)
      o << ' ' << *d.constraint;

    return o;
  }

  bool
  operator== (const dependency& x, const dependency& y)
  {
    return x.name () == y.name () && x.constraint == y.constraint;
  }

  bool
  operator!= (const dependency& x, const dependency& y)
  {
    return !(x == y);
  }

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
           optional<email_type> be,
           dependencies_type dp,
           requirements_type rq,
           optional<path> lc,
           optional<string> sh,
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
        build_email (move (be)),
        dependencies (move (dp)),
        requirements (move (rq)),
        internal_repository (move (rp)),
        location (move (lc)),
        sha256sum (move (sh))
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
    other_repositories.emplace_back (move (rp));
  }

  weighted_text package::
  search_text () const
  {
    if (!internal ())
      return weighted_text ();

    // Derive keywords from the basic package information: name,
    // version.
    //
    //@@ What about 'stable' from cppget.org/stable? Add path of
    //   the repository to keywords? Or is it too "polluting" and
    //   we will handle it in some other way (e.g., by allowing
    //   the user to specify repo location in the drop-down box)?
    //   Probably drop-box would be better as also tells what are
    //   the available internal repositories.
    //
    string k (id.name + " " + version.string () + " " + version.string (true));

    // Add tags to keywords.
    //
    for (const auto& t: tags)
      k += " " + t;

    // Add licenses to keywords.
    //
    for (const auto& la: license_alternatives)
    {
      for (const auto& l: la)
      {
        k += " " + l;

        // If license is say LGPLv2 then LGPL is also a keyword.
        //
        size_t n (l.size ());
        if (n > 2 && l[n - 2] == 'v' && l[n - 1] >= '0' && l[n - 1] <= '9')
          k += " " + string (l, 0, n - 2);
      }
    }

    return {move (k), summary, description ? *description : "", changes};
  }

  // repository
  //
  repository::
  repository (repository_location l,
              string d,
              repository_location h,
              optional<certificate_type> c,
              uint16_t r)
      : name (l.canonical_name ()),
        location (move (l)),
        display_name (move (d)),
        priority (r),
        cache_location (move (h)),
        certificate (move (c)),
        internal (true)
  {
  }

  repository::
  repository (repository_location l)
      : name (l.canonical_name ()),
        location (move (l)),
        priority (0),
        internal (false)
  {
  }
}
