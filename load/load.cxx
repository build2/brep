// file      : load/load.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <signal.h> // signal()

#include <cerrno>
#include <cstring>   // strncmp()
#include <iostream>
#include <algorithm> // find(), find_if()

#include <odb/session.hxx>
#include <odb/database.hxx>
#include <odb/exceptions.hxx>
#include <odb/transaction.hxx>
#include <odb/schema-catalog.hxx>

#include <odb/pgsql/database.hxx>

#include <libbutl/pager.mxx>
#include <libbutl/sha256.mxx>
#include <libbutl/process.mxx>
#include <libbutl/fdstream.mxx>
#include <libbutl/filesystem.mxx>
#include <libbutl/tab-parser.mxx>
#include <libbutl/manifest-parser.mxx>

#include <libbpkg/manifest.hxx>

#include <libbrep/package.hxx>
#include <libbrep/package-odb.hxx>
#include <libbrep/database-lock.hxx>

#include <load/load-options.hxx>

using std::cout;
using std::cerr;
using std::endl;

using namespace odb::core;
using namespace butl;
using namespace bpkg;
using namespace brep;

// Operation failed, diagnostics has already been issued.
//
struct failed {};

static const char* help_info (
  "  info: run 'brep-load --help' for more information");

static const path packages     ("packages.manifest");
static const path repositories ("repositories.manifest");

struct internal_repository
{
  repository_location location;
  string display_name;
  repository_location cache_location;
  optional<string> fingerprint;

  path
  packages_path () const {return cache_location.path () / packages;}

  path
  repositories_path () const {return cache_location.path () / repositories;}
};

using internal_repositories = vector<internal_repository>;

// Parse loadtab file.
//
// loadtab consists of lines in the following format:
//
// <remote-repository-location> <display-name> cache:<local-repository-location> [fingerprint:<fingerprint>]
//
// Note that if the remote repository location is a pkg repository, then the
// repository cache should be its local copy. Otherwise, the cache directory
// is expected to contain just repositories.manifest and packages.manifest
// files as dumped by bpkg-rep-info, for example:
//
// $ bpkg rep-info --manifest
//   --repositories-file repositories.manifest
//   --packages-file     packages.manifest
//   <remote-repository-location>
//
// Specifically, the packages.manifest is not a pkg package manifest list. It
// contains a raw list of package manifests that may contain values forbidden
// for the pkg package manifest list (description-file, changes-file) and may
// omit the required ones (sha256sum).
//
// @@ Latter, we may also want to support loading bpkg repositories using
//    manifest files produced by bpkg-rep-info command. This, in particular,
//    will allow handling CI requests for bpkg repositories.
//
//    The current thinking is that the CI handler will be able to "suggest"
//    this using (the planned) cache:dir+file:// form.
//
static internal_repositories
load_repositories (path p)
{
  internal_repositories repos;

  if (p.relative ())
    p.complete ();

  try
  {
    ifdstream ifs (p);
    tab_parser parser (ifs, p.string ());

    tab_fields tl;
    while (!(tl = parser.next ()).empty ())
    {
      size_t n (tl.size ()); // Fields count.
      size_t i (0);          // The field currently being processed.

      // Report an error for the field currently being processed. If i == n
      // then we refer to the end-of-line column (presumably reporting a missed
      // field).
      //
      auto bad_line = [&p, &tl, &i, n] (const string& d, size_t offset = 0)
      {
        // Offset beyond the end-of-line is meaningless.
        //
        assert (i < n || (i == n && offset == 0));

        cerr << p << ':' << tl.line << ':'
             << (i == n
                 ? tl.end_column
                 : tl[i].column + offset)
             << ": error: " << d << endl;

        throw failed ();
      };

      internal_repository r;

      try
      {
        r.location = repository_location (tl[i].value);
      }
      catch (const invalid_argument& e)
      {
        bad_line (e.what ());
      }

      if (r.location.local ())
        bad_line ("local repository location");

      for (const auto& rp: repos)
        if (rp.location.canonical_name () == r.location.canonical_name ())
          bad_line ("duplicate canonical name");

      // Display name field is a required one.
      //
      if (++i == n)
        bad_line ("no display name found");

      r.display_name = move (tl[i++].value);

      // Parse options, that have <name>:<value> form. Currently defined
      // options are cache (mandatory for now) and fingerprint.
      //
      for (; i < n; ++i)
      {
        string nv (tl[i].value);
        size_t vp;

        if (strncmp (nv.c_str (), "cache:", vp = 6) == 0)
        {
          if (!r.cache_location.empty ())
            bad_line ("cache option redefinition");

          try
          {
            // If the internal repository cache path is relative, then
            // calculate its absolute path. Such path is considered to be
            // relative to the configuration file directory path so result is
            // independent from whichever directory is current for the loader
            // process. Note that the resulting absolute path should be a valid
            // repository location.
            //
            dir_path cache_path (string (nv, vp));
            if (cache_path.relative ())
              cache_path = p.directory () / cache_path;

            // A non-pkg repository cache is not a real repository (see
            // above). We create the location of the dir type for such a cache
            // to distinguish it when it comes to the manifest files parsing.
            //
            r.cache_location = repository_location (
              repository_url (cache_path.string ()),
              r.location.type () == repository_type::pkg
              ? r.location.type ()
              : repository_type::dir);

            // Created from the absolute path repository location can not be
            // other than absolute.
            //
            assert (r.cache_location.absolute ());
          }
          catch (const invalid_path& e)     // Thrown by dir_path().
          {
            bad_line (string ("invalid cache path: ") + e.what ());
          }
          catch (const invalid_argument& e) // Thrown by repository_*().
          {
            bad_line (string ("invalid cache path: ") + e.what ());
          }

          if (!file_exists (r.packages_path ()))
            bad_line ("packages.manifest file does not exist");

          if (!file_exists (r.repositories_path ()))
            bad_line ("repositories.manifest file does not exist");
        }
        else if (strncmp (nv.c_str (), "fingerprint:", vp = 12) == 0)
        {
          if (r.fingerprint)
            bad_line ("fingerprint option redefinition");

          r.fingerprint = string (nv, vp);

          // Sanity check.
          //
          if (!r.fingerprint->empty ())
          {
            try
            {
              fingerprint_to_sha256 (*r.fingerprint);
            }
            catch (const invalid_argument&)
            {
              bad_line ("invalid fingerprint");
            }
          }
        }
        else
          bad_line ("invalid option '" + nv + "'");
      }

      // For now cache option is mandatory.
      //
      if (r.cache_location.empty ())
        bad_line ("no cache option found");

      repos.emplace_back (move (r));
    }
  }
  catch (const tab_parsing& e)
  {
    cerr << e << endl;
    throw failed ();
  }
  catch (const io_error& e)
  {
    cerr << "error: unable to read " << p << ": " << e << endl;
    throw failed ();
  }

  return repos;
}

// Check if repositories persistent state is outdated. If any repository
// differes from its persistent state or there is a persistent repository
// which is not listed in configuration file then the whole persistent
// state will be recreated. Will consider optimization later when the
// package model, including search related objects, settles down.
//
static bool
changed (const string& tenant,
         const internal_repositories& repos,
         database& db)
{
  strings names;
  for (const internal_repository& r: repos)
  {
    shared_ptr<repository> pr (
      db.find<repository> (repository_id (tenant,
                                          r.location.canonical_name ())));

    if (pr == nullptr                                                     ||
        r.location.string () != pr->location.string ()                    ||
        r.display_name != pr->display_name                                ||
        r.cache_location.path () != pr->cache_location.path ()            ||
        file_mtime (r.packages_path ()) != pr->packages_timestamp         ||
        file_mtime (r.repositories_path ()) != pr->repositories_timestamp ||
        !pr->internal)
      return true;

    names.emplace_back (r.location.canonical_name ());
  }

  using query = query<repository>;

  // Check if there is an internal repository not being listed in the
  // configuration file.
  //
  return
    !db.query<repository> (
      query::id.tenant == tenant &&
      query::internal            &&
      !query::id.canonical_name.in_range (names.begin (),
                                          names.end ())).empty ();
}

// Start 'bpkg rep-info [options] <repository_location>' process.
//
static process
repository_info (const options& lo, const string& rl, const cstrings& options)
{
  cstrings args {lo.bpkg ().string ().c_str (), "rep-info"};

  args.insert (args.end (), options.begin (), options.end ());

  for (const string& o: lo.bpkg_option ())
    args.push_back (o.c_str ());

  args.push_back (rl.c_str ());
  args.push_back (nullptr);

  try
  {
    return process (args.data (), 0, -1, 2);
  }
  catch (const process_error& e)
  {
    cerr << "error: unable to execute " << args[0] << ": " << e << endl;

    if (e.child)
      exit (1);

    throw failed ();
  }
}

// Load the repository packages from the packages.manifest file and persist
// the repository. Should be called once per repository.
//
static void
load_packages (const shared_ptr<repository>& rp, database& db)
{
  // packages_timestamp other than timestamp_nonexistent signals the
  // repository packages are already loaded.
  //
  assert (rp->packages_timestamp == timestamp_nonexistent);

  // Only locally accessible repositories allowed until package manager API is
  // ready.
  //
  assert (!rp->cache_location.empty ());

  vector<package_manifest> pms;
  const repository_location& cl (rp->cache_location);

  path p (cl.path () / packages);

  try
  {
    ifdstream ifs (p);
    rp->packages_timestamp = file_mtime (p);

    manifest_parser mp (ifs, p.string ());

    // If the repository cache directory is not a pkg repository, then the
    // packages.manifest file it contains is a raw list of the package
    // manifests that we need to parse manually (see above).
    //
    if (cl.type () != repository_type::pkg)
    {
      // We put no restrictions on the manifest values presence since it's not
      // critical for displaying and building if the packages omit some
      // manifest values (see libbpkg/manifest.hxx for details). Note, though,
      // that we expect package dependency constraints to be complete.
      //
      for (manifest_name_value nv (mp.next ()); !nv.empty (); nv = mp.next ())
        pms.emplace_back (
          mp,
          move (nv),
          false /* ignore_unknown */,
          false /* complete_depends */,
          package_manifest_flags::forbid_incomplete_depends);
    }
    else
      pms = pkg_package_manifests (mp);
  }
  catch (const io_error& e)
  {
    cerr << "error: unable to read " << p << ": " << e << endl;
    throw failed ();
  }

  for (auto& pm: pms)
  {
    shared_ptr<package> p (
      db.find<package> (package_id (rp->tenant, pm.name, pm.version)));

    // sha256sum should always be present if the package manifest comes from
    // the packages.manifest file belonging to the pkg repository.
    //
    assert (pm.sha256sum || cl.type () != repository_type::pkg);

    if (p == nullptr)
    {
      if (rp->internal)
      {
        // Create internal package object.
        //
        optional<string> dsc;
        if (pm.description)
        {
          // The description value should not be of the file type if the
          // package manifest comes from the pkg repository.
          //
          assert (!pm.description->file || cl.type () != repository_type::pkg);

          if (!pm.description->file)
            dsc = move (pm.description->text);
        }

        string chn;
        for (auto& c: pm.changes)
        {
          // The changes value should not be of the file type if the package
          // manifest comes from the pkg repository.
          //
          assert (!c.file || cl.type () != repository_type::pkg);

          if (!c.file)
          {
            if (chn.empty ())
              chn = move (c.text);
            else
            {
              if (chn.back () != '\n')
                chn += '\n'; // Always have a blank line as a separator.

              chn += "\n" + c.text;
            }
          }
        }

        dependencies ds;

        for (auto& pda: pm.dependencies)
        {
          // Ignore special build2 and bpkg dependencies. We may not have
          // packages for them and also showing them for every package is
          // probably not very helpful.
          //
          if (pda.buildtime && !pda.empty ())
          {
            const package_name& n (pda.front ().name);
            if (n == "build2" || n == "bpkg")
              continue;
          }

          ds.emplace_back (pda.conditional, pda.buildtime, move (pda.comment));

          for (auto& pd: pda)
            // The package member will be assigned during dependency
            // resolution procedure.
            //
            ds.back ().push_back ({move (pd.name),
                                   move (pd.constraint),
                                   nullptr /* package */});
        }

        // Cache before the package name is moved.
        //
        package_name project (pm.effective_project ());

        p = make_shared<package> (
          move (pm.name),
          move (pm.version),
          move (project),
          pm.priority ? move (*pm.priority) : priority (),
          move (pm.summary),
          move (pm.license_alternatives),
          move (pm.tags),
          move (dsc),
          move (chn),
          move (pm.url),
          move (pm.doc_url),
          move (pm.src_url),
          move (pm.package_url),
          move (pm.email),
          move (pm.package_email),
          move (pm.build_email),
          move (pm.build_warning_email),
          move (pm.build_error_email),
          move (ds),
          move (pm.requirements),
          move (pm.builds),
          move (pm.build_constraints),
          move (pm.location),
          move (pm.fragment),
          move (pm.sha256sum),
          rp);
      }
      else
        // Create external package object.
        //
        p = make_shared<package> (move (pm.name), move (pm.version), rp);

      db.persist (p);
    }
    else
    {
      // As soon as internal repositories get loaded first, the internal
      // package can duplicate an internal package only.
      //
      assert (!rp->internal || p->internal ());

      // Note that the sha256sum manifest value can only be present if the
      // package comes from the pkg repository.
      //
      if (rp->internal && pm.sha256sum)
      {
        // Save the package sha256sum if it is not present yet, match
        // otherwise.
        //
        if (!p->sha256sum)
          p->sha256sum = move (pm.sha256sum);
        else if (*pm.sha256sum != *p->sha256sum)
          cerr << "warning: sha256sum mismatch for package " << p->name
               << " " << p->version << endl
               << "  info: " << p->internal_repository.load ()->location
               << " has " << *p->sha256sum << endl
               << "  info: " << rp->location << " has " << *pm.sha256sum
               << endl;
      }

      p->other_repositories.push_back (rp);
      db.update (p);
    }
  }

  db.persist (rp); // Save the repository state.
}

// Load the repository manifest values from the repositories.manifest file.
// Unless this is a shallow load, also load prerequsite repositories and
// their complements state. Update the repository persistent state to save
// changed members. Should be called once per persisted internal repository.
//
static void
load_repositories (const shared_ptr<repository>& rp,
                   database& db,
                   bool shallow)
{
  // repositories_timestamp other than timestamp_nonexistent signals that
  // repository prerequisites are already loaded.
  //
  assert (rp->repositories_timestamp == timestamp_nonexistent);

  // Only locally accessible repositories allowed until package manager API is
  // ready.
  //
  assert (!rp->cache_location.empty ());

  const string& tenant (rp->tenant);

  // Repository is already persisted by the load_packages() function call.
  //
  assert (db.find<repository> (
            repository_id (tenant, rp->canonical_name)) != nullptr);

  pkg_repository_manifests rpm;

  path p (rp->cache_location.path () / repositories);

  try
  {
    ifdstream ifs (p);
    rp->repositories_timestamp = file_mtime (p);

    manifest_parser mp (ifs, p.string ());
    rpm = pkg_repository_manifests (mp);
  }
  catch (const io_error& e)
  {
    cerr << "error: unable to read " << p << ": " << e << endl;
    throw failed ();
  }

  for (auto& rm: rpm)
  {
    if (rm.effective_role () == repository_role::prerequisite &&
        !rp->internal)
      continue; // Ignore the external repository prerequisite entry.

    if (rm.effective_role () == repository_role::base)
    {
      assert (rp->location.remote () && !rp->interface_url);

      // Update the base repository with manifest values.
      //
      rp->interface_url = rm.effective_url (rp->location);

      // @@ Should we throw if url is not available for external repository ?
      //    Can, basically, repository be available on the web but have no web
      //    interface associated ?
      //
      //    Yes, there can be no web interface. So we should just not form
      //    links to packages from such repos.
      //
      if (rp->interface_url)
      {
        // Normalize web interface url adding trailing '/' if not present.
        //
        auto& u (*rp->interface_url);
        assert (!u.empty ());
        if (u.back () != '/')
          u += '/';
      }

      if (rp->internal)
      {
        rp->email = move (rm.email);
        rp->summary = move (rm.summary);
        rp->description = move (rm.description);

        // Mismatch of the repository manifest and the certificate information
        // can be the result of racing condition.
        //
        // @@ Need to address properly while fully moving to the bpkg-based
        //    fetching.
        // @@ Shouldn't we dedicate a specific exit code for such situations?
        //
        if (static_cast<bool> (rm.certificate) !=
            static_cast<bool> (rp->certificate))
        {
          cerr << "error: signing status mismatch for internal repository "
               << rp->location << endl
               << "  info: try again" << endl;

          throw failed ();
        }

        if (rm.certificate)
          rp->certificate->pem = move (*rm.certificate);
      }

      continue;
    }

    // Load prerequisite or complement repository unless this is a shallow
    // load.
    //
    if (shallow)
      continue;

    assert (!rm.location.empty ());

    repository_location rl;

    auto bad_location = [&rp, &rm] ()
    {
      cerr << "error: invalid prerequisite repository location "
           << rm.location << endl
           << "  info: base (internal) repository location is "
           << rp->location << endl;

      throw failed ();
    };

    try
    {
      // Absolute path location make no sense for the web interface.
      //
      if (rm.location.absolute ())
        bad_location ();

      // Convert the relative repository location to remote one, leave remote
      // location unchanged.
      //
      rl = repository_location (rm.location, rp->location);
    }
    catch (const invalid_argument&)
    {
      bad_location ();
    }

    const auto& cn (rl.canonical_name ());

    // Add repository to prerequisites or complements member of the dependent
    // repository.
    //
    auto& rs (rm.effective_role () == repository_role::prerequisite
              ? rp->prerequisites
              : rp->complements);

    rs.emplace_back (db, repository_id (tenant, cn));

    shared_ptr<repository> pr (
      db.find<repository> (repository_id (tenant, cn)));

    if (pr != nullptr)
      // The prerequisite repository is already loaded.
      //
      continue;

    pr = make_shared<repository> (tenant, move (rl));

    // If the prerequsite repository location is a relative path, then
    // calculate its cache location.
    //
    if (rm.location.relative ())
    {
      try
      {
        pr->cache_location =
          repository_location (rm.location, rp->cache_location);
      }
      catch (const invalid_argument&)
      {
        cerr << "error: can't obtain cache location for prerequisite "
             << "repository '" << rm.location << "'" << endl
             << "  info: base (internal) repository location is "
             << rp->location << endl
             << "  info: base repository cache location is "
             << rp->cache_location << endl;

        throw failed ();
      }
    }

    load_packages (pr, db);
    load_repositories (pr, db, false /* shallow */);
  }

  db.update (rp);
}

// Check if the package is available from the specified repository,
// its prerequisite repositories, or one of their complements,
// recursively.
//
static bool
find (const lazy_shared_ptr<repository>& r,
      const package& p,
      bool prereq = true)
{
  assert (r != nullptr);

  const auto& o (p.other_repositories);
  if (r == p.internal_repository ||
      find (o.begin (), o.end (), r) != o.end ())
    return true;

  auto rp (r.load ());
  for (const auto& cr: rp->complements)
  {
    if (find (lazy_shared_ptr<repository> (cr), p, false))
      return true;
  }

  if (prereq)
  {
    for (auto pr: rp->prerequisites)
    {
      if (find (lazy_shared_ptr<repository> (pr), p, false))
        return true;
    }
  }

  return false;
}

// Resolve package dependencies. Make sure that the best matching dependency
// belongs to the package repositories, their immediate prerequisite
// repositories, or their complements, recursively. Should be called once per
// internal package.
//
static void
resolve_dependencies (package& p, database& db)
{
  // Resolve dependencies for internal packages only.
  //
  assert (p.internal ());

  if (p.dependencies.empty ())
    return;

  for (auto& da: p.dependencies)
  {
    for (auto& d: da)
    {
      // Dependency should not be resolved yet.
      //
      assert (d.package == nullptr);

      using query = query<package>;
      query q (query::id.name == d.name);
      const auto& vm (query::id.version);

      if (d.constraint)
      {
        auto c (*d.constraint);

        assert (c.complete ());

        query qs (compare_version_eq (vm, wildcard_version, false));

        if (c.min_version && c.max_version &&
            *c.min_version == *c.max_version)
        {
          const version& v (*c.min_version);
          q = q && (compare_version_eq (vm, v, v.revision != 0) || qs);
        }
        else
        {
          query qr (true);

          if (c.min_version)
          {
            const version& v (*c.min_version);

            if (c.min_open)
              qr = compare_version_gt (vm, v, v.revision != 0);
            else
              qr = compare_version_ge (vm, v, v.revision != 0);
          }

          if (c.max_version)
          {
            const version& v (*c.max_version);

            if (c.max_open)
              qr = qr && compare_version_lt (vm, v, v.revision != 0);
            else
              qr = qr && compare_version_le (vm, v, v.revision != 0);
          }

          q = q && (qr || qs);
        }
      }

      for (const auto& pp: db.query<package> (q + order_by_version_desc (vm)))
      {
        if (find (p.internal_repository, pp))
        {
          d.package.reset (db, pp.id);
          break;
        }
      }

      if (d.package == nullptr)
      {
        cerr << "error: can't resolve dependency " << d << " of the package "
             << p.name << " " << p.version << endl
             << "  info: repository "
             << p.internal_repository.load ()->location
             << " appears to be broken" << endl;

        // Practically it is enough to resolve at least one dependency
        // alternative to build a package. Meanwhile here we consider an error
        // specifying in the manifest file an alternative which can't be
        // resolved.
        //
        throw failed ();
      }
    }
  }

  db.update (p); // Update the package state.
}

using package_ids = vector<package_id>;

// Make sure the package dependency chain doesn't contain the package id.
// Throw failed otherwise. Continue the chain with the package id and call
// itself recursively for each prerequisite of the package. Should be called
// once per internal package.
//
// @@ This should probably be eventually moved to bpkg.
//
static void
detect_dependency_cycle (const package_id& id,
                         package_ids& chain,
                         database& db)
{
  // Package of one version depending on the same package of another version
  // is something obscure. So the comparison is made up to a package name.
  //
  auto pr = [&id] (const package_id& i) -> bool {return i.name == id.name;};
  auto i (find_if (chain.begin (), chain.end (), pr));

  if (i != chain.end ())
  {
    cerr << "error: package dependency cycle: ";

    auto prn = [&db] (const package_id& id)
    {
      shared_ptr<package> p (db.load<package> (id));
      assert (p->internal () || !p->other_repositories.empty ());

      shared_ptr<repository> r (
        p->internal ()
        ? p->internal_repository.load ()
        : p->other_repositories[0].load ());

      cerr << p->name << " " << p->version << " (" << r->canonical_name << ")";
    };

    for (; i != chain.end (); ++i)
    {
      prn (*i);
      cerr << " -> ";
    }

    prn (id);
    cerr << endl;
    throw failed ();
  }

  chain.push_back (id);

  shared_ptr<package> p (db.load<package> (id));
  for (const auto& da: p->dependencies)
  {
    for (const auto& d: da)
      detect_dependency_cycle (d.package.object_id (), chain, db);
  }

  chain.pop_back ();
}

// Return the certificate information for a signed repository and nullopt for
// an unsigned. Note that a repository at the remote location is not trusted
// unless the certificate fingerprint is provided (which also means it should
// either be signed or the wildcard fingerprint specified). A local repository
// location is, instead, trusted by default. If the fingerprint is provided
// then the repository is authenticated regardless of the location type.
//
static optional<certificate>
certificate_info (const options& lo,
                  const repository_location& rl,
                  const optional<string>& fp)
{
  try
  {
    cstrings args {
      "--cert-fingerprint",
      "--cert-name",
      "--cert-organization",
      "--cert-email",
      "-q"};                 // Don't print info messages.

    const char* trust ("--trust-no");

    if (fp)
    {
      if (!fp->empty ())
      {
        args.push_back ("--trust");
        args.push_back (fp->c_str ());
      }
      else
        trust = "--trust-yes";

      if (!rl.remote ())
      {
        args.push_back ("--auth");
        args.push_back ("all");
      }
    }

    args.push_back (trust);

    process pr (repository_info (lo, rl.string (), args));

    try
    {
      ifdstream is (
        move (pr.in_ofd),
        ifdstream::failbit | ifdstream::badbit | ifdstream::eofbit);

      optional<certificate> cert;

      string fingerprint;
      getline (is, fingerprint);

      if (!fingerprint.empty ())
      {
        cert = certificate ();
        cert->fingerprint = move (fingerprint);
        getline (is, cert->name);
        getline (is, cert->organization);
        getline (is, cert->email);
      }
      else
      {
        // Read out empty lines.
        //
        string s;
        getline (is, s); // Name.
        getline (is, s); // Organization.
        getline (is, s); // Email.
      }

      // Check that EOF is successfully reached.
      //
      is.exceptions (ifdstream::failbit | ifdstream::badbit);
      if (is.peek () != ifdstream::traits_type::eof ())
        throw io_error ("");

      is.close ();

      if (pr.wait ())
        return cert;

      // Fall through.
      //
    }
    catch (const io_error&)
    {
      // Child exit status doesn't matter. Just wait for the process
      // completion and fall through.
      //
      pr.wait ();
    }

    // Assume the child issued diagnostics.
    //
    cerr << "error: unable to fetch certificate information for "
         << rl.canonical_name () << endl;

    // Fall through.
  }
  catch (const process_error& e)
  {
    cerr << "error: unable to fetch certificate information for "
         << rl.canonical_name () << ": " << e << endl;

    // Fall through.
  }

  throw failed ();
}

int
main (int argc, char* argv[])
try
{
  // On POSIX ignore SIGPIPE which is signaled to a pipe-writing process if
  // the pipe reading end is closed. Note that by default this signal
  // terminates a process. Also note that there is no way to disable this
  // behavior on a file descriptor basis or for the write() function call.
  //
  if (signal (SIGPIPE, SIG_IGN) == SIG_ERR)
  {
    cerr << "error: unable to ignore broken pipe (SIGPIPE) signal: "
         << system_error (errno, generic_category ()) << endl; // Sanitize.
    return 1;
  }

  cli::argv_scanner scan (argc, argv, true);
  options ops (scan);

  // Version.
  //
  if (ops.version ())
  {
    cout << "brep-load " << BREP_VERSION_ID << endl
         << "libbrep " << LIBBREP_VERSION_ID << endl
         << "libbbot " << LIBBBOT_VERSION_ID << endl
         << "libbpkg " << LIBBPKG_VERSION_ID << endl
         << "libbutl " << LIBBUTL_VERSION_ID << endl
         << "Copyright (c) 2014-2019 Code Synthesis Ltd" << endl
         << "This is free software released under the MIT license." << endl;

    return 0;
  }

  // Help.
  //
  if (ops.help ())
  {
    pager p ("brep-load help",
             false,
             ops.pager_specified () ? &ops.pager () : nullptr,
             &ops.pager_option ());

    print_usage (p.stream ());

    // If the pager failed, assume it has issued some diagnostics.
    //
    return p.wait () ? 0 : 1;
  }

  if (argc < 2)
  {
    cerr << "error: configuration file expected" << endl
         << help_info << endl;
    return 1;
  }

  if (argc > 2)
  {
    cerr << "error: unexpected argument encountered" << endl
         << help_info << endl;
    return 1;
  }

  // By default the tenant is empty and assumes a single-tenant mode. Let's
  // require the specified tenant to be non-empty.
  //
  const string& tnt (ops.tenant ());

  if (ops.tenant_specified () && tnt.empty ())
  {
    cerr << "error: empty tenant" << endl
         << help_info << endl;
    return 1;
  }

  odb::pgsql::database db (
    ops.db_user (),
    ops.db_password (),
    ops.db_name (),
    ops.db_host (),
    ops.db_port (),
    "options='-c default_transaction_isolation=serializable'");

  // Prevent several brep-load/migrate instances from updating DB
  // simultaneously.
  //
  database_lock l (db);

  transaction t (db.begin ());

  // Check that the package database schema matches the current one.
  //
  const string ds ("package");
  if (schema_catalog::current_version (db, ds) != db.schema_version (ds))
  {
    cerr << "error: package database schema differs from the current one"
         << endl << "  info: use brep-migrate to migrate the database" << endl;
    return 1;
  }

  // Load the description of all the internal repositories from the
  // configuration file.
  //
  internal_repositories irs (load_repositories (path (argv[1])));

  if (ops.force () || changed (tnt, irs, db))
  {
    // Rebuild repositories persistent state from scratch.
    //
    // Note that in the single-tenant mode the tenant must be empty. In the
    // multi-tenant mode all tenants must be non-empty. So in the
    // single-tenant mode we erase all database objects (possibly from
    // multiple tenants). Otherwise, cleanup the specified and the empty
    // tenants only.
    //
    if (tnt.empty ())                // Single-tenant mode.
    {
      db.erase_query<package> ();
      db.erase_query<repository> ();
      db.erase_query<tenant> ();
    }
    else                             // Multi-tenant mode.
    {
      cstrings ts ({tnt.c_str (), ""});

      db.erase_query<package> (
        query<package>::id.tenant.in_range (ts.begin (), ts.end ()));

      db.erase_query<repository> (
        query<repository>::id.tenant.in_range (ts.begin (), ts.end ()));

      db.erase_query<tenant> (
        query<tenant>::id.in_range (ts.begin (), ts.end ()));
    }

    // Persist the tenant.
    //
    db.persist (tenant (tnt));

    // On the first pass over the internal repositories we load their
    // certificate information and packages.
    //
    uint16_t priority (1);
    for (const auto& ir: irs)
    {
      optional<certificate> cert;

      if (ir.location.type () == repository_type::pkg)
        cert = certificate_info (
          ops,
          !ir.cache_location.empty () ? ir.cache_location : ir.location,
          ir.fingerprint);

      shared_ptr<repository> r (
        make_shared<repository> (tnt,
                                 ir.location,
                                 move (ir.display_name),
                                 move (ir.cache_location),
                                 move (cert),
                                 priority++));

      load_packages (r, db);
    }

    // On the second pass over the internal repositories we load their
    // (not yet loaded) manifest values, complement, and prerequisite
    // repositories.
    //
    for (const auto& ir: irs)
    {
      shared_ptr<repository> r (
        db.load<repository> (
          repository_id (tnt, ir.location.canonical_name ())));

      load_repositories (r, db, ops.shallow ());
    }

    // Resolve internal packages dependencies unless this is a shallow load.
    //
    if (!ops.shallow ())
    {
      session s;
      using query = query<package>;

      for (auto& p:
             db.query<package> (
               query::id.tenant == tnt &&
               query::internal_repository.canonical_name.is_not_null ()))
        resolve_dependencies (p, db);

      // Make sure there is no package dependency cycles.
      //
      package_ids chain;
      for (const auto& p:
             db.query<package> (
               query::id.tenant == tnt &&
               query::internal_repository.canonical_name.is_not_null ()))
        detect_dependency_cycle (p.id, chain, db);
    }
  }

  t.commit ();
  return 0;
}
catch (const database_locked&)
{
  cerr << "brep-load or brep-migrate is running" << endl;
  return 2;
}
catch (const recoverable& e)
{
  cerr << "recoverable database error: " << e << endl;
  return 3;
}
catch (const cli::exception& e)
{
  cerr << "error: " << e << endl << help_info << endl;
  return 1;
}
catch (const failed&)
{
  return 1; // Diagnostics has already been issued.
}
// Fully qualified to avoid ambiguity with odb exception.
//
catch (const std::exception& e)
{
  cerr << "error: " << e << endl;
  return 1;
}
