// file      : libbrep/package.cxx -*- C++ -*-
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
  ostream&
  operator<< (ostream& o, const dependency& d)
  {
    o << d.name;

    if (d.constraint)
      o << ' ' << *d.constraint;

    return o;
  }

  bool
  operator== (const dependency& x, const dependency& y)
  {
    return x.name == y.name && x.constraint == y.constraint;
  }

  bool
  operator!= (const dependency& x, const dependency& y)
  {
    return !(x == y);
  }

  // tenant
  //
  tenant::
  tenant (string i,
          bool p,
          optional<string> r,
          optional<tenant_service> s)
      : id (move (i)),
        private_ (p),
        interactive (move (r)),
        creation_timestamp (timestamp::clock::now ()),
        service (move (s))
  {
  }

  // package
  //
  package::
  package (package_name nm,
           version_type vr,
           optional<string> uv,
           package_name pn,
           priority_type pr,
           string sm,
           license_alternatives_type la,
           small_vector<string, 5> tp,
           small_vector<string, 5> kw,
           optional<typed_text> ds,
           optional<typed_text> pds,
           optional<typed_text> ch,
           optional<manifest_url> ur,
           optional<manifest_url> du,
           optional<manifest_url> su,
           optional<manifest_url> pu,
           optional<email_type> em,
           optional<email_type> pe,
           optional<email_type> be,
           optional<email_type> bwe,
           optional<email_type> bee,
           dependencies_type dp,
           requirements_type rq,
           small_vector<test_dependency, 1> ts,
           build_class_exprs bs,
           build_constraints_type bc,
           build_package_configs bcs,
           optional<path> lc,
           optional<string> fr,
           optional<string> sh,
           shared_ptr<repository_type> rp)
      : id (rp->tenant, move (nm), vr),
        tenant (id.tenant),
        name (id.name),
        version (move (vr)),
        upstream_version (move (uv)),
        project (move (pn)),
        priority (move (pr)),
        summary (move (sm)),
        license_alternatives (move (la)),
        topics (move (tp)),
        keywords (move (kw)),
        description (move (ds)),
        package_description (move (pds)),
        changes (move (ch)),
        url (move (ur)),
        doc_url (move (du)),
        src_url (move (su)),
        package_url (move (pu)),
        email (move (em)),
        package_email (move (pe)),
        build_email (move (be)),
        build_warning_email (move (bwe)),
        build_error_email (move (bee)),
        dependencies (move (dp)),
        requirements (move (rq)),
        tests (move (ts)),
        builds (move (bs)),
        build_constraints (move (bc)),
        internal_repository (move (rp)),
        location (move (lc)),
        fragment (move (fr)),
        sha256sum (move (sh))
  {
    // Add the default build configuration at the beginning, unless it is
    // specified explicitly.
    //
    if (find_if (bcs.begin (), bcs.end (),
                 [] (const build_package_config& c)
                 {return c.name == "default";}) != bcs.end ())
    {
      build_configs = move (bcs);
    }
    else
    {
      build_configs.reserve (bcs.size () + 1);
      build_configs.emplace_back ("default");
      build_configs.insert (build_configs.end (),
                            make_move_iterator (bcs.begin ()),
                            make_move_iterator (bcs.end ()));
    }

    if (stub ())
      unbuildable_reason = brep::unbuildable_reason::stub;
    else if (!internal_repository->buildable)
      unbuildable_reason = brep::unbuildable_reason::unbuildable;

    buildable = !unbuildable_reason;

    assert (internal_repository->internal);
  }

  package::
  package (package_name nm,
           version_type vr,
           build_class_exprs bs,
           build_constraints_type bc,
           shared_ptr<repository_type> rp)
      : id (rp->tenant, move (nm), vr),
        tenant (id.tenant),
        name (id.name),
        version (move (vr)),
        builds (move (bs)),
        build_constraints (move (bc)),
        buildable (false),
        unbuildable_reason (stub ()
                            ? brep::unbuildable_reason::stub
                            : brep::unbuildable_reason::external)
  {
    assert (!rp->internal);
    other_repositories.emplace_back (move (rp));
  }

  weighted_text package::
  search_text () const
  {
    if (!internal ())
      return weighted_text ();

    // Derive search keywords from the basic package information: project,
    // name, and version.
    //
    //@@ What about 'stable' from cppget.org/stable? Add path of
    //   the repository to keywords? Or is it too "polluting" and
    //   we will handle it in some other way (e.g., by allowing
    //   the user to specify repo location in the drop-down box)?
    //   Probably drop-box would be better as also tells what are
    //   the available internal repositories.
    //
    string k (project.string () + ' ' + name.string () + ' ' +
              version.string () + ' ' + version.string (true));

    if (upstream_version)
      k += ' ' + *upstream_version;

    // Add licenses to search keywords.
    //
    for (const auto& la: license_alternatives)
    {
      for (const auto& l: la)
      {
        k += ' ' + l;

        // If license is say LGPLv2 then LGPL is also a search keyword.
        //
        size_t n (l.size ());
        if (n > 2 && l[n - 2] == 'v' && l[n - 1] >= '0' && l[n - 1] <= '9')
          k += ' ' + string (l, 0, n - 2);
      }
    }

    // Derive second-strongest search keywords from the package summary.
    //
    string k2 (summary);

    // Add topics to the second-strongest search keywords.
    //
    for (const auto& t: topics)
      k2 += ' ' + t;

    // Add keywords to the second-strongest search keywords.
    //
    for (const auto& k: keywords)
      k2 += ' ' + k;

    string d (description ? description->text : "");

    if (package_description)
    {
      if (description)
        d += ' ';

      d += package_description->text;
    }

    return {move (k), move (k2), move (d), changes ? changes->text : ""};
  }

  // repository
  //
  repository::
  repository (string t,
              repository_location l,
              string d,
              repository_location h,
              optional<certificate_type> c,
              bool b,
              uint16_t r)
      : id (move (t), l.canonical_name ()),
        tenant (id.tenant),
        canonical_name (id.canonical_name),
        location (move (l)),
        display_name (move (d)),
        priority (r),
        cache_location (move (h)),
        certificate (move (c)),
        internal (true),
        buildable (b)
  {
  }

  repository::
  repository (string t, repository_location l)
      : id (move (t), l.canonical_name ()),
        tenant (id.tenant),
        canonical_name (id.canonical_name),
        location (move (l)),
        priority (0),
        internal (false),
        buildable (false)
  {
  }
}
