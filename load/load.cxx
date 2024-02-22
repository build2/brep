// file      : load/load.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <signal.h> // signal()

#include <cerrno>
#include <chrono>
#include <thread>   // this_thread::sleep_for()
#include <cstring>  // strncmp()
#include <iostream>

#include <odb/session.hxx>
#include <odb/database.hxx>
#include <odb/exceptions.hxx>
#include <odb/transaction.hxx>
#include <odb/schema-catalog.hxx>

#include <odb/pgsql/database.hxx>

#include <libbutl/pager.hxx>
#include <libbutl/sha256.hxx>
#include <libbutl/process.hxx>
#include <libbutl/fdstream.hxx>
#include <libbutl/filesystem.hxx>
#include <libbutl/tab-parser.hxx>
#include <libbutl/manifest-parser.hxx>

#include <libbpkg/manifest.hxx>

#include <libbrep/package.hxx>
#include <libbrep/package-odb.hxx>
#include <libbrep/database-lock.hxx>

#include <load/load-options.hxx>

using std::cout;
using std::cerr;
using std::endl;

using namespace std::this_thread;
using namespace odb::core;
using namespace butl;
using namespace bpkg;
using namespace brep;

using manifest_name_values = vector<manifest_name_value>;

// Operation failed, diagnostics has already been issued.
//
struct failed {};

static const char* help_info (
  "  info: run 'brep-load --help' for more information");

static const path packages     ("packages.manifest");
static const path repositories ("repositories.manifest");

// Retry executing bpkg on recoverable errors for about 10 seconds.
//
// Should we just exit with some "bpkg recoverable" code instead and leave it
// to the caller to perform retries? Feels like it's better to handle such
// errors ourselves rather than to complicate every caller. Note that having
// some frequently updated prerequisite repository can make these errors quite
// probable, even if the internal repositories are rarely updated.
//
static const size_t               bpkg_retries (10);
static const std::chrono::seconds bpkg_retry_timeout (1);

struct internal_repository
{
  repository_location location;
  string display_name;
  repository_location cache_location;
  optional<string> fingerprint;
  bool buildable = true;

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
// <remote-repository-location> <display-name> cache:<local-repository-location> [fingerprint:<fingerprint>] [buildable:(yes|no)]
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
// omit the required ones (sha256sum, description-type).
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
      // options are cache (mandatory for now), fingerprint, and buildable.
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
        else if (strncmp (nv.c_str (), "buildable:", vp = 10) == 0)
        {
          string v (nv, vp);

          r.buildable = (v == "yes");

          if (!r.buildable && v != "no")
            bad_line ("invalid buildable option value");
        }
        else
          bad_line ("invalid option '" + nv + '\'');
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
        r.buildable != pr->buildable                                      ||
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
load_packages (const shared_ptr<repository>& rp,
               const repository_location& cl,
               database& db,
               bool ignore_unknown,
               const manifest_name_values& overrides,
               const string& overrides_name)
{
  // packages_timestamp other than timestamp_nonexistent signals the
  // repository packages are already loaded.
  //
  assert (rp->packages_timestamp == timestamp_nonexistent);

  vector<package_manifest> pms;

  assert (!cl.empty ());

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
      // that we expect dependency constraints to be complete.
      //
      for (manifest_name_value nv (mp.next ()); !nv.empty (); nv = mp.next ())
        pms.emplace_back (
          mp,
          move (nv),
          ignore_unknown,
          false /* complete_values */,
          package_manifest_flags::forbid_incomplete_values);
    }
    else
      pms = pkg_package_manifests (mp, ignore_unknown);
  }
  catch (const io_error& e)
  {
    cerr << "error: unable to read " << p << ": " << e << endl;
    throw failed ();
  }

  using brep::dependency;
  using brep::dependency_alternative;
  using brep::dependency_alternatives;

  for (package_manifest& pm: pms)
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
        if (!overrides.empty ())
        try
        {
          pm.override (overrides, overrides_name);
        }
        catch (const manifest_parsing& e)
        {
          cerr << "error: unable to override " << p << " manifest: " << e
               << endl;

          throw failed ();
        }

        // Create internal package object.
        //
        // Return nullopt if the text is in a file (can happen if the
        // repository is of a type other than pkg) or if the type is not
        // recognized (can only happen in the "ignore unknown" mode).
        //
        auto to_typed_text = [&cl, ignore_unknown] (typed_text_file&& v)
        {
          optional<typed_text> r;

          // The description value should not be of the file type if the
          // package manifest comes from the pkg repository.
          //
          assert (!v.file || cl.type () != repository_type::pkg);

          if (!v.file)
          {
            // Cannot throw since the manifest parser has already verified the
            // effective type in the same "ignore unknown" mode.
            //
            optional<text_type> t (v.effective_type (ignore_unknown));

            // If the description type is unknown (which may be the case for
            // some "transitional" period and only if --ignore-unknown is
            // specified) we just silently drop the description.
            //
            assert (t || ignore_unknown);

            if (t)
              r = typed_text {move (v.text), *t};
          }

          return r;
        };

        // Convert descriptions.
        //
        optional<typed_text> ds (
          pm.description
          ? to_typed_text (move (*pm.description))
          : optional<typed_text> ());

        optional<typed_text> pds (
          pm.package_description
          ? to_typed_text (move (*pm.package_description))
          : optional<typed_text> ());

        // Merge changes into a single typed text object.
        //
        // If the text type is not recognized for any changes entry or some
        // entry refers to a file, then assume that no changes are specified.
        //
        optional<typed_text> chn;

        for (auto& c: pm.changes)
        {
          optional<typed_text> tc (to_typed_text (move (c)));

          if (!tc)
          {
            chn = nullopt;
            break;
          }

          if (!chn)
          {
            chn = move (*tc);
          }
          else
          {
            // Should have failed while parsing the manifest otherwise.
            //
            assert (tc->type == chn->type);

            string& v (chn->text);

            assert (!v.empty ()); // Changes manifest value cannot be empty.

            if (v.back () != '\n')
              v += '\n'; // Always have a blank line as a separator.

            v += '\n';
            v += tc->text;
          }
        }

        dependencies tds;

        for (auto& das: pm.dependencies)
        {
          dependency_alternatives tdas (das.buildtime, move (das.comment));

          for (auto& da: das)
          {
            dependency_alternative tda (move (da.enable),
                                        move (da.reflect),
                                        move (da.prefer),
                                        move (da.accept),
                                        move (da.require));

            for (auto& d: da)
            {
              package_name& n (d.name);

              // Ignore special build2 and bpkg dependencies. We may not have
              // packages for them and also showing them for every package is
              // probably not very helpful.
              //
              if (das.buildtime && (n == "build2" || n == "bpkg"))
                continue;

              // The package member will be assigned during dependency
              // resolution procedure.
              //
              tda.push_back (dependency {move (n),
                                         move (d.constraint),
                                         nullptr /* package */});
            }

            if (!tda.empty ())
              tdas.push_back (move (tda));
          }

          if (!tdas.empty ())
            tds.push_back (move (tdas));
        }

        small_vector<brep::test_dependency, 1> ts;

        if (!pm.tests.empty ())
        {
          ts.reserve (pm.tests.size ());

          for (bpkg::test_dependency& td: pm.tests)
            ts.emplace_back (move (td.name),
                             td.type,
                             td.buildtime,
                             move (td.constraint),
                             move (td.enable),
                             move (td.reflect));
        }

        // Cache before the package name is moved.
        //
        package_name project (pm.effective_project ());

        p = make_shared<package> (
          move (pm.name),
          move (pm.version),
          move (pm.upstream_version),
          move (project),
          pm.priority ? move (*pm.priority) : priority (),
          move (pm.summary),
          move (pm.license_alternatives),
          move (pm.topics),
          move (pm.keywords),
          move (ds),
          move (pds),
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
          move (tds),
          move (pm.requirements),
          move (ts),
          move (pm.builds),
          move (pm.build_constraints),
          move (pm.build_configs),
          move (pm.location),
          move (pm.fragment),
          move (pm.sha256sum),
          rp);
      }
      else
        // Create external package object.
        //
        p = make_shared<package> (move (pm.name),
                                  move (pm.version),
                                  move (pm.builds),
                                  move (pm.build_constraints),
                                  rp);

      db.persist (p);
    }
    else
    {
      // As soon as internal repositories get loaded first, the internal
      // package can duplicate an internal package only.
      //
      assert (!rp->internal || p->internal ());

      if (rp->internal)
      {
        // Note that the sha256sum manifest value can only be present if the
        // package comes from the pkg repository.
        //
        if (pm.sha256sum)
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

        // A non-stub package is buildable if belongs to at least one
        // buildable repository (see libbrep/package.hxx for details).
        // Note that if this is an external test package it will be marked as
        // unbuildable later (see resolve_dependencies() for details).
        //
        if (rp->buildable && !p->buildable && !p->stub ())
        {
          p->buildable = true;
          p->unbuildable_reason = nullopt;
        }
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
load_repositories (const options& lo,
                   const shared_ptr<repository>& rp,
                   const repository_location& cl,
                   database& db,
                   bool ignore_unknown,
                   bool shallow)
{
  // repositories_timestamp other than timestamp_nonexistent signals that
  // repository prerequisites are already loaded.
  //
  assert (rp->repositories_timestamp == timestamp_nonexistent);

  const string& tenant (rp->tenant);

  // Repository is already persisted by the load_packages() function call.
  //
  assert (db.find<repository> (
            repository_id (tenant, rp->canonical_name)) != nullptr);

  pkg_repository_manifests rpm;

  assert (!cl.empty ());

  path p (cl.path () / repositories);

  try
  {
    ifdstream ifs (p);
    rp->repositories_timestamp = file_mtime (p);

    manifest_parser mp (ifs, p.string ());
    rpm = pkg_repository_manifests (mp, ignore_unknown);

    if (rpm.empty ())
      rpm.emplace_back (repository_manifest ()); // Add the base repository.
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

    // If the base repository is internal and the prerequsite repository
    // location is a relative path, then calculate its cache location.
    //
    if (rp->internal && rm.location.relative ())
    {
      // For an internal repository the cache location always comes from the
      // loadtab file.
      //
      assert (cl.path () == rp->cache_location.path ());

      try
      {
        pr->cache_location = repository_location (rm.location, cl);
      }
      catch (const invalid_argument&)
      {
        cerr << "error: can't obtain cache location for prerequisite "
             << "repository '" << rm.location << "'" << endl
             << "  info: base (internal) repository location is "
             << rp->location << endl
             << "  info: base repository cache location is " << cl << endl;

        throw failed ();
      }
    }

    // If the (external) prerequisite repository cache location is empty, then
    // check if the repository is local and, if that's the case, use its
    // location as a cache location. Otherwise, fetch the repository
    // information creating a temporary cache for it.
    //
    auto_rmdir cdr;         // Remove the temporary cache after the repo load.
    repository_location cl; // Repository temporary cache location.

    if (pr->cache_location.empty ())
    {
      if (pr->location.local ())
      {
        pr->cache_location = pr->location;
      }
      else
      {
        dir_path cd;

        try
        {
          cd = dir_path::temp_path ("brep-load-cache");
        }
        catch (const system_error& e)
        {
          cerr << "unable to obtain temporary directory: " << e;
          throw failed ();
        }

        // It's highly unlikely but still possible that the temporary cache
        // directory already exists. This can only happen due to the unclean
        // loader termination. Let's remove it and retry.
        //
        try
        {
          if (try_mkdir (cd) == mkdir_status::already_exists)
          {
            try_rmdir_r (cd);

            if (try_mkdir (cd) == mkdir_status::already_exists)
              throw_generic_error (EEXIST);
          }
        }
        catch (const system_error& e)
        {
          cerr << "unable to create directory '" << cd << "': " << e;
          throw failed ();
        }

        cdr = auto_rmdir (cd);

        path rf (cd / repositories);
        path pf (cd / packages);

        // Note that the fetch timeout can be overridden via --bpkg-option.
        //
        cstrings args {
          "--fetch-timeout", "60", // 1 minute.
          "--deep",
          "--manifest",
          "--repositories",
          "--repositories-file", rf.string ().c_str (),
          "--packages",
          "--packages-file", pf.string ().c_str ()};

        if (rm.trust)
        {
          args.push_back ("--trust");
          args.push_back (rm.trust->c_str ());
        }

        // Always add it, so bpkg won't try to prompt for a certificate
        // authentication if the fingerprint doesn't match.
        //
        args.push_back ("--trust-no");

        // Retry bpkg-rep-info on recoverable errors, for a while.
        //
        for (size_t i (0);; ++i)
        {
          if (i != 0)
          {
            // Let's follow up the bpkg's diagnostics with the number of
            // retries left.
            //
            cerr << bpkg_retries - i + 1 << " retries left" << endl;
            sleep_for (bpkg_retry_timeout);
          }

          process p (repository_info (lo, pr->location.string (), args));

          try
          {
            // Bail out from the retry loop on success.
            //
            if (p.wait ())
              break;

            // Assume the child issued diagnostics if terminated normally.
            //
            if (p.exit->normal ())
            {
              // Retry the manifests fetch on a recoverable error, unless the
              // retries limit is reached.
              //
              if (p.exit->code () == 2 && i != bpkg_retries)
                continue;
            }
            else
              cerr << "process " << lo.bpkg () << " " << *p.exit << endl;

            cerr << "error: unable to fetch manifests for "
                 << pr->canonical_name << endl
                 << "  info: base repository location is "
                 << rp->location << endl;

            throw failed ();
          }
          catch (const process_error& e)
          {
            cerr << "error: unable to fetch manifests for "
                 << pr->canonical_name << ": " << e << endl;

            throw failed ();
          }
        }

        // Note that this is a non-pkg repository cache and so we create the
        // dir repository location (see load_repositories(path) for details).
        //
        cl = repository_location (repository_url (cd.string ()),
                                  repository_type::dir);
      }
    }

    // We don't apply overrides to the external packages.
    //
    load_packages (pr,
                   !pr->cache_location.empty () ? pr->cache_location : cl,
                   db,
                   ignore_unknown,
                   manifest_name_values () /* overrides */,
                   "" /* overrides_name */);

    load_repositories (lo,
                       pr,
                       !pr->cache_location.empty () ? pr->cache_location : cl,
                       db,
                       ignore_unknown,
                       false /* shallow */);
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

// Resolve package run-time dependencies and external tests. Make sure that
// the best matching dependency belongs to the package repositories, their
// complements, recursively, or their immediate prerequisite repositories
// (only for run-time dependencies). Set the buildable flag to false for the
// resolved external tests packages. Fail if unable to resolve a dependency,
// unless ignore_unresolved is true in which case leave this dependency
// NULL. Should be called once per internal package.
//
static void
resolve_dependencies (package& p, database& db, bool ignore_unresolved)
{
  using brep::dependency;
  using brep::dependency_alternative;
  using brep::dependency_alternatives;

  // Resolve dependencies for internal packages only.
  //
  assert (p.internal ());

  if (p.dependencies.empty () && p.tests.empty ())
    return;

  auto resolve = [&p, &db] (dependency& d, bool test)
  {
    // Dependency should not be resolved yet.
    //
    assert (d.package == nullptr);

    using query = query<package>;
    query q (query::id.name == d.name);
    const auto& vm (query::id.version);

    if (d.constraint)
    {
      const version_constraint& c (*d.constraint);

      assert (c.complete ());

      query qs (compare_version_eq (vm,
                                    canonical_version (wildcard_version),
                                    false /* revision */));

      if (c.min_version && c.max_version && *c.min_version == *c.max_version)
      {
        const version& v (*c.min_version);
        q = q && (compare_version_eq (vm,
                                      canonical_version (v),
                                      v.revision.has_value ()) ||
                  qs);
      }
      else
      {
        query qr (true);

        if (c.min_version)
        {
          const version& v (*c.min_version);
          canonical_version cv (v);
          bool rv (v.revision);

          if (c.min_open)
            qr = compare_version_gt (vm, cv, rv);
          else
            qr = compare_version_ge (vm, cv, rv);
        }

        if (c.max_version)
        {
          const version& v (*c.max_version);
          canonical_version cv (v);
          bool rv (v.revision);

          if (c.max_open)
            qr = qr && compare_version_lt (vm, cv, rv);
          else
            qr = qr && compare_version_le (vm, cv, rv);
        }

        q = q && (qr || qs);
      }
    }

    for (const auto& pp: db.query<package> (q + order_by_version_desc (vm)))
    {
      if (find (p.internal_repository, pp, !test))
      {
        d.package.reset (db, pp.id);

        // If the resolved dependency is an external test, then mark it as
        // such, unless it is a stub.
        //
        if (test)
        {
          shared_ptr<package> dp (d.package.load ());

          if (!dp->stub ())
          {
            dp->buildable = false;
            dp->unbuildable_reason = unbuildable_reason::test;

            db.update (dp);
          }
        }

        return true;
      }
    }

    return false;
  };

  auto bail = [&p] (const dependency& d, const char* what)
  {
    cerr << "error: can't resolve " << what << " " << d << " for the package "
         << p.name << " " << p.version << endl
         << "  info: repository " << p.internal_repository.load ()->location
         << " appears to be broken" << endl;

    throw failed ();
  };

  for (dependency_alternatives& das: p.dependencies)
  {
    // Practically it is enough to resolve at least one dependency alternative
    // to build a package. Meanwhile here we consider an error specifying in
    // the manifest file an alternative which can't be resolved, unless
    // unresolved dependencies are allowed.
    //
    for (dependency_alternative& da: das)
    {
      for (dependency& d: da)
      {
        if (!resolve (d, false /* test */) && !ignore_unresolved)
          bail (d, "dependency");
      }
    }
  }

  for (brep::test_dependency& td: p.tests)
  {
    if (!resolve (td, true /* test */) && !ignore_unresolved)
      bail (td, td.name.string ().c_str ());
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
  for (const auto& das: p->dependencies)
  {
    for (const auto& da: das)
    {
      for (const auto& d: da)
        detect_dependency_cycle (d.package.object_id (), chain, db);
    }
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

  // Retry bpkg-rep-info on recoverable errors, for a while.
  //
  for (size_t i (0);; ++i)
  {
    if (i != 0)
    {
      // Let's follow up the bpkg's diagnostics with the number of retries
      // left.
      //
      cerr << bpkg_retries - i + 1 << " retries left" << endl;
      sleep_for (bpkg_retry_timeout);
    }

    try
    {
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

      // Assume the child issued diagnostics if terminated normally.
      //
      if (pr.exit->normal ())
      {
        // Retry the certificate fetch on a recoverable error, unless the
        // retries limit is reached.
        //
        if (pr.exit->code () == 2 && i != bpkg_retries)
          continue;
      }
      else
        cerr << "process " << lo.bpkg () << " " << *pr.exit << endl;

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
    throw failed ();
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
         << "Copyright (c) " << BREP_COPYRIGHT << "." << endl
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
    throw failed ();
  }

  if (argc > 2)
  {
    cerr << "error: unexpected argument encountered" << endl
         << help_info << endl;
    throw failed ();
  }

  // By default the tenant is empty and assumes a single-tenant mode. Let's
  // require the specified tenant to be non-empty.
  //
  const string& tnt (ops.tenant ());

  if (ops.tenant_specified () && tnt.empty ())
  {
    cerr << "error: empty tenant" << endl
         << help_info << endl;
    throw failed ();
  }

  // Verify the --service-* options.
  //
  if (ops.service_id_specified ())
  {
    if (!ops.tenant_specified ())
    {
      cerr << "error: --service-id requires --tenant" << endl;
      throw failed ();
    }

    if (ops.service_type ().empty ())
    {
      cerr << "error: --service-id requires --service-type"
           << endl;
      throw failed ();
    }
  }
  else
  {
    if (ops.service_type_specified ())
    {
      cerr << "error: --service-type requires --service-id"
           << endl;
      throw failed ();
    }

    if (ops.service_data_specified ())
    {
      cerr << "error: --service-data requires --service-id"
           << endl;
      throw failed ();
    }
  }

  // Parse and validate overrides, if specified.
  //
  // Note that here we make sure that the overrides manifest is valid.
  // Applying overrides to a specific package manifest may still fail (see
  // package_manifest::validate_overrides() for details).
  //
  manifest_name_values overrides;

  if (ops.overrides_file_specified ())
  try
  {
    const string& name (ops.overrides_file ().string ());

    ifdstream is (ops.overrides_file ());
    manifest_parser mp (is, name);
    overrides = parse_manifest (mp);
    is.close ();

    package_manifest::validate_overrides (overrides, name);
  }
  catch (const manifest_parsing& e)
  {
    cerr << "error: unable to parse overrides: " << e << endl;
    throw failed ();
  }
  catch (const io_error& e)
  {
    cerr << "error: unable to read '" << ops.overrides_file () << "': " << e
         << endl;
    throw failed ();
  }

  odb::pgsql::database db (
    ops.db_user (),
    ops.db_password (),
    ops.db_name (),
    ops.db_host (),
    ops.db_port (),
    "options='-c default_transaction_isolation=serializable'");

  // Prevent several brep utility instances from updating the package database
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
    throw failed ();
  }

  // Note: the interactive tenant implies private.
  //
  if (ops.interactive_specified ())
    ops.private_ (true);

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
    // Note that if the tenant service is specified and some tenant with the
    // same service id and type is already persisted, then we will end up with
    // the `object already persistent` error and terminate with the exit code
    // 1 (fatal error). We could potentially dedicate a special exit code for
    // such a case, so that the caller may recognize it and behave accordingly
    // (CI request handler can treat it as a client error rather than an
    // internal error, etc). However, let's first see if it ever becomes a
    // problem.
    //
    optional<tenant_service> service;

    if (ops.service_id_specified ())
      service = tenant_service (ops.service_id (),
                                ops.service_type (),
                                (ops.service_data_specified ()
                                 ? ops.service_data ()
                                 : optional<string> ()));

    db.persist (tenant (tnt,
                        ops.private_ (),
                        (ops.interactive_specified ()
                         ? ops.interactive ()
                         : optional<string> ()),
                        move (service)));

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
                                 ir.buildable,
                                 priority++));

      load_packages (r,
                     r->cache_location,
                     db,
                     ops.ignore_unknown (),
                     overrides,
                     ops.overrides_file ().string ());
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

      load_repositories (ops,
                         r,
                         r->cache_location,
                         db,
                         ops.ignore_unknown (),
                         ops.shallow ());
    }

    // Resolve internal packages dependencies and, unless this is a shallow
    // load, make sure there are no package dependency cycles.
    //
    {
      session s;
      using query = query<package>;

      for (auto& p:
             db.query<package> (
               query::id.tenant == tnt &&
               query::internal_repository.canonical_name.is_not_null ()))
        resolve_dependencies (p, db, ops.shallow ());

      if (!ops.shallow ())
      {
        package_ids chain;
        for (const auto& p:
               db.query<package> (
                 query::id.tenant == tnt &&
                 query::internal_repository.canonical_name.is_not_null ()))
          detect_dependency_cycle (p.id, chain, db);
      }
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
