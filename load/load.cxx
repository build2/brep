// file      : load/load.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <signal.h> // signal()

#include <map>
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
#include <libbutl/openssl.hxx>
#include <libbutl/fdstream.hxx>
#include <libbutl/filesystem.hxx>
#include <libbutl/tab-parser.hxx>
#include <libbutl/manifest-parser.hxx>

#include <libbpkg/manifest.hxx>

#include <libbrep/package.hxx>
#include <libbrep/package-odb.hxx>
#include <libbrep/database-lock.hxx>
#include <libbrep/review-manifest.hxx>

#include <load/load-options.hxx>
#include <load/options-types.hxx>

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
static const path reviews      ("reviews.manifest");

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

// Map of package versions to their metadata information in the form it is
// stored in the database (reviews summary, etc).
//
// This map is filled by recursively traversing the metadata directory and
// parsing the encountered metadata manifest files (reviews.manifest, etc; see
// --metadata option for background on metadata). Afterwards, this map is used
// as a data source for the being persisted/updated package objects.
//
struct package_version_key
{
  package_name  name;
  brep::version version;

  package_version_key (package_name n, brep::version v)
      : name (move (n)), version (move (v)) {}

  bool
  operator< (const package_version_key& k) const
  {
    if (int r = name.compare (k.name))
      return r < 0;

    return version < k.version;
  }
};

class package_version_metadata
{
public:
  // Extracted from the package metadata directory. Must match the respective
  // package manifest information.
  //
  package_name project;

  optional<reviews_summary> reviews;

  // The directory the metadata manifest files are located. It has the
  // <project>/<package>/<version> form and is only used for diagnostics.
  //
  dir_path
  directory () const
  {
    assert (reviews); // At least one kind of metadata must be present.
    return reviews->manifest_file.directory ();
  }
};

using package_version_metadata_map = std::map<package_version_key,
                                              package_version_metadata>;

// Load the repository packages from the packages.manifest file and persist
// the repository. Should be called once per repository.
//
static void
load_packages (const options& lo,
               const shared_ptr<repository>& rp,
               const repository_location& cl,
               database& db,
               bool ignore_unknown,
               const manifest_name_values& overrides,
               const string& overrides_name,
               optional<package_version_metadata_map>& metadata)
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

  const string& tenant (rp->tenant);

  for (package_manifest& pm: pms)
  {
    shared_ptr<package> p (
      db.find<package> (package_id (tenant, pm.name, pm.version)));

    // sha256sum should always be present if the package manifest comes from
    // the packages.manifest file belonging to the pkg repository.
    //
    assert (pm.sha256sum || cl.type () != repository_type::pkg);

    if (p == nullptr)
    {
      // Apply the package manifest overrides.
      //
      if (!overrides.empty ())
      try
      {
        pm.override (overrides, overrides_name);
      }
      catch (const manifest_parsing& e)
      {
        cerr << "error: unable to override " << pm.name << ' ' << pm.version
             << " manifest: " << e << endl;

        throw failed ();
      }

      // Convert the package manifest build configurations (contain public
      // keys data) into the brep's build package configurations (contain
      // public key object lazy pointers). Keep the bot key lists empty if
      // the package is not buildable.
      //
      package_build_configs build_configs;

      if (!pm.build_configs.empty ())
      {
        build_configs.reserve (pm.build_configs.size ());

        for (bpkg::build_package_config& c: pm.build_configs)
        {
          build_configs.emplace_back (move (c.name),
                                      move (c.arguments),
                                      move (c.comment),
                                      move (c.builds),
                                      move (c.constraints),
                                      move (c.auxiliaries),
                                      package_build_bot_keys (),
                                      move (c.email),
                                      move (c.warning_email),
                                      move (c.error_email));
        }
      }

      if (rp->internal)
      {
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

        // If the package is buildable, then save the package manifest's
        // common and build configuration-specific bot keys into the database
        // and translate the key data lists into the lists of the public key
        // object lazy pointers.
        //
        package_build_bot_keys bot_keys;

        if (rp->buildable)
        {
          // Save the specified bot keys into the database as public key
          // objects, unless they are already persisted. Translate these keys
          // into the public key object lazy pointers.
          //
          auto keys_to_objects = [&lo,
                                  &pm,
                                  &tenant,
                                  &db] (strings&& keys)
          {
            package_build_bot_keys r;

            if (keys.empty ())
              return r;

            r.reserve (keys.size ());

            for (string& key: keys)
            {
              // Calculate the key fingerprint.
              //
              string fp;

              try
              {
                openssl os (path ("-"), path ("-"), 2,
                            lo.openssl (),
                            "pkey",
                            lo.openssl_option (), "-pubin", "-outform", "DER");

                os.out << key;
                os.out.close ();

                fp = sha256 (os.in).string ();
                os.in.close ();

                if (!os.wait ())
                {
                  cerr << "process " << lo.openssl () << ' ' << *os.exit
                       << endl;

                  throw io_error ("");
                }
              }
              catch (const io_error&)
              {
                cerr << "error: unable to convert custom build bot public key "
                     << "for package " << pm.name << ' ' << pm.version << endl
                     << "  info: key:" << endl
                     << key << endl;

                throw failed ();
              }
              catch (const process_error& e)
              {
                cerr << "error: unable to convert custom build bot public key "
                     << "for package " << pm.name << ' ' << pm.version << ": "
                     << e << endl;

                throw failed ();
              }

              // Try to find the public_key object for the calculated
              // fingerprint. If it doesn't exist, then create and persist the
              // new object.
              //
              public_key_id id (tenant, move (fp));
              shared_ptr<public_key> k (db.find<public_key> (id));

              if (k == nullptr)
              {
                k = make_shared<public_key> (move (id.tenant),
                                             move (id.fingerprint),
                                             move (key));

                db.persist (k);
              }

              r.push_back (move (k));
            }

            return r;
          };

          bot_keys = keys_to_objects (move (pm.build_bot_keys));

          assert (build_configs.size () == pm.build_configs.size ());

          for (size_t i (0); i != build_configs.size (); ++i)
            build_configs[i].bot_keys =
              keys_to_objects (move (pm.build_configs[i].bot_keys));
        }

        optional<reviews_summary> rvs;

        if (metadata)
        {
          auto i (metadata->find (package_version_key {pm.name, pm.version}));

          if (i != metadata->end ())
          {
            package_version_metadata& md (i->second);

            if (md.project != project)
            {
              cerr << "error: project '" << project << "' of package "
                   << pm.name << ' ' << pm.version << " doesn't match "
                   << "metadata directory path "
                   << lo.metadata () / md.directory ();

              throw failed ();
            }

            if (md.reviews)
              rvs = move (md.reviews);
          }
        }

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
          move (pm.build_auxiliaries),
          move (bot_keys),
          move (build_configs),
          move (rvs),
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
                                  move (pm.build_auxiliaries),
                                  move (build_configs),
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
        // buildable repository (see libbrep/package.hxx for details). Note
        // that if this is an external test package it will be marked as
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
    optional<package_version_metadata_map> metadata;

    load_packages (lo,
                   pr,
                   !pr->cache_location.empty () ? pr->cache_location : cl,
                   db,
                   ignore_unknown,
                   manifest_name_values () /* overrides */,
                   "" /* overrides_name */,
                   metadata);

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

// Try to resolve package regular dependencies and external tests. Make sure
// that the best matching dependency belongs to the package repositories,
// their complements, recursively, or their immediate prerequisite
// repositories (only for regular dependencies). Set the buildable flag to
// false for the resolved external tests packages. Leave the package member
// NULL for unresolved dependencies.
//
static void
resolve_dependencies (package& p, database& db)
{
  using brep::dependency;
  using brep::dependency_alternative;
  using brep::dependency_alternatives;
  using brep::test_dependency;

  // Resolve dependencies for internal packages only.
  //
  assert (p.internal ());

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

  // Update the package state if any dependency is resolved.
  //
  bool update (false);

  for (dependency_alternatives& das: p.dependencies)
  {
    for (dependency_alternative& da: das)
    {
      for (dependency& d: da)
      {
        if (resolve (d, false /* test */))
          update = true;
      }
    }
  }

  for (test_dependency& td: p.tests)
  {
    if (resolve (td, true /* test */))
      update = true;
  }

  if (update)
    db.update (p);
}

// Verify that the unresolved dependencies can be ignored.
//
// Specifically, fail for an unresolved regular dependency, unless
// ignore_unresolved is true or this is a conditional dependency and either
// ignore_unresolved_cond argument is 'all' or it is 'tests' and the specified
// package is a tests, examples, or benchmarks package. Fail for an unresolved
// external test, unless ignore_unresolved or ignore_unresolved_tests is
// true. If ignore_unresolved_tests is true, then remove the unresolved tests
// entry from the package manifest. Should be called once per internal package
// after resolve_dependencies() is called for all of them.
//
static void
verify_dependencies (
  package& p,
  database& db,
  bool ignore_unresolved,
  bool ignore_unresolved_tests,
  optional<ignore_unresolved_conditional_dependencies> ignore_unresolved_cond)
{
  using brep::dependency;
  using brep::dependency_alternative;
  using brep::dependency_alternatives;
  using brep::test_dependency;

  // Verify dependencies for internal packages only.
  //
  assert (p.internal ());

  auto bail = [&p] (const dependency& d, const string& what)
  {
    cerr << "error: can't resolve " << what << ' ' << d << " for the package "
         << p.name << ' ' << p.version << endl
         << "  info: repository " << p.internal_repository.load ()->location
         << " appears to be broken" << endl;

    throw failed ();
  };

  if (!ignore_unresolved)
  {
    // There must always be a reason why a package is not buildable.
    //
    assert (p.buildable || p.unbuildable_reason);

    bool test (!p.buildable &&
               *p.unbuildable_reason == unbuildable_reason::test);

    for (dependency_alternatives& das: p.dependencies)
    {
      for (dependency_alternative& da: das)
      {
        for (dependency& d: da)
        {
          if (d.package == nullptr)
          {
            if (da.enable && ignore_unresolved_cond)
            {
              switch (*ignore_unresolved_cond)
              {
              case ignore_unresolved_conditional_dependencies::all: continue;
              case ignore_unresolved_conditional_dependencies::tests:
                {
                  if (test)
                    continue;

                  break;
                }
              }
            }

            bail (d, "dependency");
          }
        }
      }
    }
  }

  if (!ignore_unresolved || ignore_unresolved_tests)
  {
    // Update the package state if any test dependency is erased.
    //
    bool update (false);

    for (auto i (p.tests.begin ()); i != p.tests.end (); )
    {
      test_dependency& td (*i);

      if (td.package == nullptr)
      {
        if (!ignore_unresolved && !ignore_unresolved_tests)
          bail (td, to_string (td.type));

        if (ignore_unresolved_tests)
        {
          i = p.tests.erase (i);
          update = true;
          continue;
        }
      }

      ++i;
    }

    if (update)
      db.update (p);
  }
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
      {
        // Skip unresolved dependencies.
        //
        if (d.package != nullptr)
          detect_dependency_cycle (d.package.object_id (), chain, db);
      }
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

  if (ops.tenant_specified ())
  {
    if (tnt.empty ())
    {
      cerr << "error: empty tenant" << endl
           << help_info << endl;
      throw failed ();
    }
  }
  else
  {
    if (ops.existing_tenant ())
    {
      cerr << "error: --existing-tenant requires --tenant" << endl
           << help_info << endl;
      throw failed ();
    }
  }

  // Verify the --service-* options.
  //
  if (ops.service_id_specified ())
  {
    if (!ops.tenant_specified ())
    {
      cerr << "error: --service-id requires --tenant" << endl
           << help_info << endl;
      throw failed ();
    }

    if (ops.service_type ().empty ())
    {
      cerr << "error: --service-id requires --service-type" << endl
           << help_info << endl;
      throw failed ();
    }
  }
  else
  {
    if (ops.service_type_specified ())
    {
      cerr << "error: --service-type requires --service-id" << endl
           << help_info << endl;
      throw failed ();
    }

    if (ops.service_data_specified ())
    {
      cerr << "error: --service-data requires --service-id" << endl
           << help_info << endl;
      throw failed ();
    }
  }

  // Note: the interactive tenant implies private.
  //
  if (ops.interactive_specified ())
    ops.private_ (true);

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

  // Load the description of all the internal repositories from the
  // configuration file.
  //
  internal_repositories irs (load_repositories (path (argv[1])));

  // Prevent several brep utility instances from updating the package database
  // simultaneously.
  //
  database_lock l (db);

  // Check that the package database schema matches the current one and if the
  // package information needs to be (re-)loaded.
  //
  bool load_pkgs;
  {
    transaction t (db.begin ());

    // Check the database schema match.
    //
    const string ds ("package");

    if (schema_catalog::current_version (db, ds) != db.schema_version (ds))
    {
      cerr << "error: package database schema differs from the current one"
           << endl << "  info: use brep-migrate to migrate the database" << endl;
      throw failed ();
    }

    load_pkgs = (ops.force () || changed (tnt, irs, db));

    t.commit ();
  }

  // Check if the package versions metadata needs to be (re-)loaded and, if
  // that's the case, stash it in the memory.
  //
  optional<package_version_metadata_map> metadata;
  if (ops.metadata_specified () && (load_pkgs || ops.metadata_changed ()))
  {
    metadata = package_version_metadata_map ();

    const dir_path& d (ops.metadata ());

    // The first level are package projects.
    //
    try
    {
      for (const dir_entry& e: dir_iterator (d, dir_iterator::no_follow))
      {
        const string& n (e.path ().string ());

        if (e.type () != entry_type::directory || n[0] == '.')
          continue;

        package_name project;

        try
        {
          project = package_name (n);
        }
        catch (const invalid_argument& e)
        {
          cerr << "error: name of subdirectory '" << n << "' in " << d
               << " is not a project name: " << e << endl;
          throw failed ();
        }

        // The second level are package names.
        //
        dir_path pd (d / path_cast<dir_path> (e.path ()));

        try
        {
          for (const dir_entry& e: dir_iterator (pd, dir_iterator::no_follow))
          {
            const string& n (e.path ().string ());

            if (e.type () != entry_type::directory || n[0] == '.')
              continue;

            package_name name;

            try
            {
              name = package_name (n);
            }
            catch (const invalid_argument& e)
            {
              cerr << "error: name of subdirectory '" << n << "' in " << pd
                   << " is not a package name: " << e << endl;
              throw failed ();
            }

            // The third level are package versions.
            //
            dir_path vd (pd / path_cast<dir_path> (e.path ()));

            try
            {
              for (const dir_entry& e: dir_iterator (vd,
                                                     dir_iterator::no_follow))
              {
                const string& n (e.path ().string ());

                if (e.type () != entry_type::directory || n[0] == '.')
                  continue;

                version ver;

                try
                {
                  ver = version (n);
                }
                catch (const invalid_argument& e)
                {
                  cerr << "error: name of subdirectory '" << n << "' in " << vd
                       << " is not a package version: " << e << endl;
                  throw failed ();
                }

                dir_path md (vd / path_cast<dir_path> (e.path ()));

                // Parse the reviews.manifest file, if present.
                //
                // Note that semantically, the absent manifest file and the
                // empty manifest list are equivalent and result in an absent
                // reviews summary.
                //
                optional<reviews_summary> rs;
                {
                  path rf (md / reviews);

                  try
                  {
                    if (file_exists (rf))
                    {
                      ifdstream ifs (rf);
                      manifest_parser mp (ifs, rf.string ());

                      // Count the passed and failed reviews.
                      //
                      size_t ps (0);
                      size_t fl (0);

                      for (review_manifest& m:
                             review_manifests (mp, ops.ignore_unknown ()))
                      {
                        bool fail (false);

                        for (const review_aspect& r: m.results)
                        {
                          switch (r.result)
                          {
                          case review_result::fail: fail = true; break;

                          case review_result::unchanged:
                            {
                              cerr << "error: unsupported review result "
                                   << "'unchanged' in " << rf << endl;
                              throw failed ();
                            }

                          case review_result::pass: break; // Noop
                          }
                        }

                        ++(fail ? fl : ps);
                      }

                      if (ps + fl != 0)
                        rs = reviews_summary {ps, fl, rf.relative (d)};
                    }
                  }
                  catch (const manifest_parsing& e)
                  {
                    cerr << "error: unable to parse reviews: " << e << endl;
                    throw failed ();
                  }
                  catch (const io_error& e)
                  {
                    cerr << "error: unable to read " << rf << ": " << e << endl;
                    throw failed ();
                  }
                  catch (const system_error& e)
                  {
                    cerr << "error: unable to stat " << rf << ": " << e << endl;
                    throw failed ();
                  }
                }

                // Add the package version metadata to the map if any kind of
                // metadata is present.
                //
                if (rs)
                {
                  (*metadata)[package_version_key {name, move (ver)}] =
                    package_version_metadata {project, move (rs)};
                }
              }
            }
            catch (const system_error& e)
            {
              cerr << "error: unable to iterate over " << vd << ": " << e
                   << endl;
              throw failed ();
            }
          }
        }
        catch (const system_error& e)
        {
          cerr << "error: unable to iterate over " << pd << ": " << e << endl;
          throw failed ();
        }
      }
    }
    catch (const system_error& e)
    {
      cerr << "error: unable to iterate over " << d << ": " << e << endl;
      throw failed ();
    }
  }

  // Bail out if no package information nor metadata needs to be loaded.
  //
  if (!load_pkgs && !metadata)
    return 0;

  transaction t (db.begin ());

  if (load_pkgs)
  {
    shared_ptr<tenant> t; // Not NULL in the --existing-tenant mode.

    // Rebuild repositories persistent state from scratch.
    //
    // Note that in the single-tenant mode the tenant must be empty. In the
    // multi-tenant mode all tenants, excluding the pre-created ones, must be
    // non-empty. So in the single-tenant mode we erase all database objects
    // (possibly from multiple tenants). Otherwise, cleanup the empty tenant
    // and, unless in the --existing-tenant mode, the specified one.
    //
    if (tnt.empty ())                // Single-tenant mode.
    {
      db.erase_query<package> ();
      db.erase_query<repository> ();
      db.erase_query<public_key> ();
      db.erase_query<tenant> ();
    }
    else                             // Multi-tenant mode.
    {
      // NOTE: don't forget to update ci_start::create() if changing anything
      // here.
      //
      cstrings ts ({""});

      // In the --existing-tenant mode make sure that the specified tenant
      // exists, is not archived, not marked as unloaded, and is
      // empty. Otherwise (not in the --existing-tenant mode), remove this
      // tenant.
      //
      if (ops.existing_tenant ())
      {
        t = db.find<tenant> (tnt);

        if (t == nullptr)
        {
          cerr << "error: unable to find tenant " << tnt << endl;
          throw failed ();
        }

        if (t->archived)
        {
          cerr << "error: tenant " << tnt << " is archived" << endl;
          throw failed ();
        }

        if (t->unloaded_timestamp)
        {
          cerr << "error: tenant " << tnt << " is marked as unloaded" << endl;
          throw failed ();
        }

        size_t n (db.query_value<repository_count> (
                    query<repository_count>::id.tenant == tnt));

        if (n != 0)
        {
          cerr << "error: tenant " << tnt << " is not empty" << endl;
          throw failed ();
        }
      }
      else
        ts.push_back (tnt.c_str ());

      db.erase_query<package> (
        query<package>::id.tenant.in_range (ts.begin (), ts.end ()));

      db.erase_query<repository> (
        query<repository>::id.tenant.in_range (ts.begin (), ts.end ()));

      db.erase_query<public_key> (
        query<public_key>::id.tenant.in_range (ts.begin (), ts.end ()));

      db.erase_query<tenant> (
        query<tenant>::id.in_range (ts.begin (), ts.end ()));
    }

    // Craft the tenant service object from the --service-* options.
    //
    // In the --existing-tenant mode make sure that the specified service
    // matches the service associated with the pre-created tenant and update
    // the service data, if specified.
    //
    optional<tenant_service> service;

    if (ops.service_id_specified ())
    {
      service = tenant_service (ops.service_id (),
                                ops.service_type (),
                                (ops.service_data_specified ()
                                 ? ops.service_data ()
                                 : optional<string> ()));

      if (ops.existing_tenant ())
      {
        assert (t != nullptr);

        if (!t->service)
        {
          cerr << "error: no service associated with tenant " << tnt << endl;
          throw failed ();
        }

        if (t->service->id != service->id || t->service->type != service->type)
        {
          cerr << "error: associated service mismatch for tenant " << tnt << endl <<
                  "  info: specified service: " << service->id << ' '
                                                << service->type << endl <<
                  "  info: associated service: " << t->service->id << ' '
                                                 << t->service->type << endl;
          throw failed ();
        }

        if (service->data)
        {
          t->service->data = move (service->data);
          db.update (t);
        }
      }
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
    if (!ops.existing_tenant ())
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

      load_packages (ops,
                     r,
                     r->cache_location,
                     db,
                     ops.ignore_unknown (),
                     overrides,
                     ops.overrides_file ().string (),
                     metadata);
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

    // Try to resolve the internal packages dependencies and verify that the
    // unresolved ones can be ignored. Unless this is a shallow load, make
    // sure there are no package dependency cycles.
    //
    {
      session s;
      using query = query<package>;

      query q (query::id.tenant == tnt &&
               query::internal_repository.canonical_name.is_not_null ());

      for (auto& p: db.query<package> (q))
        resolve_dependencies (p, db);

      for (auto& p: db.query<package> (q))
      {
        verify_dependencies (
          p,
          db,
          ops.shallow (),
          ops.ignore_unresolv_tests (),
          (ops.ignore_unresolv_cond_specified ()
           ? ops.ignore_unresolv_cond ()
           : optional<ignore_unresolved_conditional_dependencies> ()));
      }

      if (!ops.shallow ())
      {
        package_ids chain;
        for (const auto& p: db.query<package> (q))
          detect_dependency_cycle (p.id, chain, db);
      }
    }
  }
  else if (metadata)
  {
    // Iterate over the packages which contain metadata and apply the changes,
    // if present. Erase the metadata map entries which introduce such
    // changes, so at the end only the newly added metadata is left in the
    // map.
    //
    using query = query<package>;

    for (package& p: db.query<package> (query::reviews.pass.is_not_null ()))
    {
      bool u (false);
      auto i (metadata->find (package_version_key {p.name, p.version}));

      if (i == metadata->end ())
      {
        // Mark the section as loaded, so the reviews summary is updated.
        //
        p.reviews_section.load ();
        p.reviews = nullopt;
        u = true;
      }
      else
      {
        package_version_metadata& md (i->second);

        if (md.project != p.project)
        {
          cerr << "error: project '" << p.project << "' of package "
               << p.name << ' ' << p.version << " doesn't match metadata "
               << "directory path " << ops.metadata () / md.directory ();

          throw failed ();
        }

        db.load (p, p.reviews_section);

        if (p.reviews != md.reviews)
        {
          p.reviews = move (md.reviews);
          u = true;
        }

        metadata->erase (i);
      }

      if (u)
        db.update (p);
    }

    // Add the newly added metadata to the packages.
    //
    for (auto& m: *metadata)
    {
      if (shared_ptr<package> p =
          db.find<package> (package_id (tnt, m.first.name, m.first.version)))
      {
        package_version_metadata& md (m.second);

        if (m.second.project != p->project)
        {
          cerr << "error: project '" << p->project << "' of package "
               << p->name << ' ' << p->version << " doesn't match metadata "
               << "directory path " << ops.metadata () / md.directory ();

          throw failed ();
        }

        // Mark the section as loaded, so the reviews summary is updated.
        //
        p->reviews_section.load ();
        p->reviews = move (md.reviews);

        db.update (p);
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
