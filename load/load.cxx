// file      : load/load.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
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

#include <butl/pager>
#include <butl/sha256>
#include <butl/process>
#include <butl/fdstream>
#include <butl/filesystem>
#include <butl/tab-parser>
#include <butl/manifest-parser>

#include <bpkg/manifest>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <libbrep/package.hxx>
#include <libbrep/package-odb.hxx>
#include <libbrep/database-lock.hxx>

#include <load/load-options.hxx>

using namespace std;
using namespace odb::core;
using namespace butl;
using namespace bpkg;
using namespace brep;

// Operation failed, diagnostics has already been issued.
//
struct failed {};

static const char* help_info (
  "  info: run 'brep-load --help' for more information");

struct internal_repository
{
  repository_location location;
  string display_name;
  repository_location cache_location;
  optional<string> fingerprint;

  path
  packages_path () const {return cache_location.path () / path ("packages");}

  path
  repositories_path () const {
    return cache_location.path () / path ("repositories");}
};

using internal_repositories = vector<internal_repository>;

  // Parse loadtab file.
  //
  // loadtab consists of lines in the following format:
  //
  // <remote-repository-location> <display-name> cache:<local-repository-location> [fingerprint:<fingerprint>]
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
            dir_path cache_path = dir_path (string (nv, vp));
            if (cache_path.relative ())
              cache_path = p.directory () / cache_path;

            r.cache_location = repository_location (cache_path.string ());

            // Created from the absolute path repository location can not be
            // other than absolute.
            //
            assert (r.cache_location.absolute ());
          }
          catch (const invalid_path& e)     // Thrown by dir_path().
          {
            bad_line (string ("invalid cache path: ") + e.what ());
          }
          catch (const invalid_argument& e) // Thrown by repository_location().
          {
            bad_line (string ("invalid cache path: ") + e.what ());
          }

          if (!file_exists (r.packages_path ()))
            bad_line ("'packages' file does not exist");

          if (!file_exists (r.repositories_path ()))
            bad_line ("'repositories' file does not exist");
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
changed (const internal_repositories& repos, database& db)
{
  strings names;
  for (auto& r: repos)
  {
    shared_ptr<repository> pr (
      db.find<repository> (r.location.canonical_name ()));

    if (pr == nullptr || r.location.string () != pr->location.string () ||
        r.display_name != pr->display_name ||
        r.cache_location.path () != pr->cache_location.path () ||
        file_mtime (r.packages_path ()) != pr->packages_timestamp ||
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
      query::internal &&
      !query::name.in_range (names.begin (), names.end ())).empty ();
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

// Load the repository packages from the 'packages' file and persist the
// repository. Should be called once per repository.
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

  package_manifests pkm;
  path p (rp->cache_location.path () / path ("packages"));

  try
  {
    ifdstream ifs (p);
    rp->packages_timestamp = file_mtime (p);

    manifest_parser mp (ifs, p.string ());
    pkm = package_manifests (mp);
  }
  catch (const io_error& e)
  {
    cerr << "error: unable to read " << p << ": " << e << endl;
    throw failed ();
  }

  for (auto& pm: pkm)
  {
    shared_ptr<package> p (
      db.find<package> (package_id (pm.name, pm.version)));

    // sha256sum should always be present if the package manifest comes from
    // the 'packages' file.
    //
    assert (pm.sha256sum);

    if (p == nullptr)
    {
      if (rp->internal)
      {
        // Create internal package object.
        //
        optional<string> dsc;
        if (pm.description)
        {
          assert (!pm.description->file);
          dsc = move (pm.description->text);
        }

        string chn;
        for (auto& c: pm.changes)
        {
          assert (!c.file);

          if (chn.empty ())
            chn = move (c.text);
          else
          {
            if (chn.back () != '\n')
              chn += '\n'; // Always have a blank line as a separator.

            chn += "\n" + c.text;
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
            const string& n (pda.front ().name);
            if (n == "build2" || n == "bpkg")
              continue;
          }

          ds.emplace_back (pda.conditional, pda.buildtime, move (pda.comment));

          for (auto& pd: pda)
            // Proper version will be assigned during dependency resolution
            // procedure. Here we rely on the fact the foreign key constraint
            // check is deferred until the current transaction commit.
            //
            ds.back ().push_back ({
                lazy_shared_ptr<package> (
                  db, package_id (move (pd.name), version ())),
                move (pd.constraint)});
        }

        p = make_shared<package> (
          move (pm.name),
          move (pm.version),
          pm.priority ? move (*pm.priority) : priority (),
          move (pm.summary),
          move (pm.license_alternatives),
          move (pm.tags),
          move (dsc),
          move (chn),
          move (pm.url),
          move (pm.package_url),
          move (pm.email),
          move (pm.package_email),
          move (pm.build_email),
          move (ds),
          move (pm.requirements),
          move (pm.location),
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

      if (rp->internal && pm.sha256sum != p->sha256sum)
        cerr << "warning: sha256sum mismatch for package " << p->id.name
             << " " << p->version << endl
             << "  info: " << p->internal_repository.load ()->location
             << " has " << *p->sha256sum << endl
             << "  info: " << rp->location << " has " << *pm.sha256sum
             << endl;

      p->other_repositories.push_back (rp);
      db.update (p);
    }
  }

  db.persist (rp); // Save the repository state.
}

// Load the repository manifest values, prerequsite repositories, and their
// complements state from the 'repositories' file. Update the repository
// persistent state to save changed members. Should be called once per
// persisted internal repository.
//
static void
load_repositories (const shared_ptr<repository>& rp, database& db)
{
  // repositories_timestamp other than timestamp_nonexistent signals that
  // repository prerequisites are already loaded.
  //
  assert (rp->repositories_timestamp == timestamp_nonexistent);

  // Only locally accessible repositories allowed until package manager API is
  // ready.
  //
  assert (!rp->cache_location.empty ());

  // Repository is already persisted by the load_packages() function call.
  //
  assert (db.find<repository> (rp->name) != nullptr);

  repository_manifests rpm;

  path p (rp->cache_location.path () / path ("repositories"));

  try
  {
    ifdstream ifs (p);
    rp->repositories_timestamp = file_mtime (p);

    manifest_parser mp (ifs, p.string ());
    rpm = repository_manifests (mp);
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
      assert (rp->location.remote () && !rp->url);

      // Update the base repository with manifest values.
      //
      rp->url = rm.effective_url (rp->location);

      // @@ Should we throw if url is not available for external repository ?
      //    Can, basically, repository be available on the web but have no web
      //    interface associated ?
      //
      //    Yes, there can be no web interface. So we should just not form
      //    links to packages from such repos.
      //
      if (rp->url)
      {
        // Normalize web interface url adding trailing '/' if not present.
        //
        auto& u (*rp->url);
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

    // Load prerequisite or complement repository.
    //
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
      rl = repository_location (rm.location.string (), rp->location);
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

    rs.emplace_back (db, cn);

    shared_ptr<repository> pr (db.find<repository> (cn));

    if (pr != nullptr)
      // The prerequisite repository is already loaded.
      //
      continue;

    pr = make_shared<repository> (move (rl));

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
    load_repositories (pr, db);
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

// Resolve package dependencies. Ensure that the best matching dependency
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
      assert (d.package.object_id ().version.empty ());

      using query = query<package>;
      query q (query::id.name == d.name ());
      const auto& vm (query::id.version);

      if (d.constraint)
      {
        auto c (*d.constraint);
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

      if (d.package.object_id ().version.empty ())
      {
        cerr << "error: can't resolve dependency " << d << " of the package "
             << p.id.name << " " << p.version << endl
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

// Ensure the package dependency chain do not contain the package id. Throw
// failed otherwise. Continue the chain with the package id and call itself
// recursively for each prerequisite of the package. Should be called once per
// internal package.
//
// @@ This should probably be eventually moved to bpkg.
//
static void
detect_dependency_cycle (
  const package_id& id, package_ids& chain, database& db)
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

      cerr << id.name << " " << p->version << " (" << r->name << ")";
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
    cout << "brep-load " << BREP_VERSION_STR << endl
         << "libbrep " << LIBBREP_VERSION_STR << endl
         << "libbbot " << LIBBBOT_VERSION_STR << endl
         << "libbpkg " << LIBBPKG_VERSION_STR << endl
         << "libbutl " << LIBBUTL_VERSION_STR << endl
         << "Copyright (c) 2014-2017 Code Synthesis Ltd" << endl
         << "MIT; see accompanying LICENSE file" << endl;

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
    cerr << "error: configuration file path argument expected" << endl
         << help_info << endl;
    return 1;
  }

  if (argc > 2)
  {
    cerr << "error: unexpected argument encountered" << endl
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

  // Check that the database 'package' schema matches the current one.
  //
  const string ds ("package");
  if (schema_catalog::current_version (db, ds) != db.schema_version (ds))
  {
    cerr << "error: database 'package' schema differs from the current one"
         << endl << "  info: use brep-migrate to migrate the database" << endl;
    return 1;
  }

  // Load the description of all the internal repositories from the
  // configuration file.
  //
  internal_repositories irs (load_repositories (path (argv[1])));

  if (changed (irs, db))
  {
    // Rebuild repositories persistent state from scratch.
    //
    db.erase_query<package> ();
    db.erase_query<repository> ();

    // On the first pass over the internal repositories we load their
    // certificate information and packages.
    //
    uint16_t priority (1);
    for (const auto& ir: irs)
    {
      optional<certificate> cert (
        certificate_info (
          ops,
          !ir.cache_location.empty () ? ir.cache_location : ir.location,
          ir.fingerprint));

      shared_ptr<repository> r (
        make_shared<repository> (ir.location,
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
        db.load<repository> (ir.location.canonical_name ()));

      load_repositories (r, db);
    }

    session s;
    using query = query<package>;

    // Resolve internal packages dependencies.
    //
    for (auto& p:
           db.query<package> (query::internal_repository.is_not_null ()))
      resolve_dependencies (p, db);

    // Ensure there is no package dependency cycles.
    //
    package_ids chain;
    for (const auto& p:
           db.query<package> (query::internal_repository.is_not_null ()))
      detect_dependency_cycle (p.id, chain, db);
  }

  t.commit ();
  return 0;
}
catch (const database_locked&)
{
  cerr << "brep-load or brep-migrate instance is running" << endl;
  return 2;
}
catch (const recoverable& e)
{
  cerr << "database recoverable error: " << e << endl;
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
