// file      : load/load.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <sstream>
#include <fstream>
#include <ostream>
#include <iostream>
#include <stdexcept> // runtime_error, invalid_argument
#include <algorithm> // find(), find_if()

#include <odb/session.hxx>
#include <odb/database.hxx>
#include <odb/transaction.hxx>
#include <odb/schema-catalog.hxx>

#include <odb/pgsql/database.hxx>

#include <butl/filesystem>

#include <bpkg/manifest-parser> // manifest_parsing

#include <brep/types>
#include <brep/utility>
#include <brep/version>

#include <brep/package>
#include <brep/package-odb>
#include <brep/database-lock>

#include <load/options>

using namespace std;
using namespace odb::core;
using namespace butl;
using namespace bpkg;
using namespace brep;

static void
usage (ostream& os)
{
  os << "Usage: brep-load [options] <file>" << endl
     << "File lists internal repositories." << endl
     << "Options:" << endl;

  options::print_usage (os);
}

static inline bool
space (char c) noexcept
{
  return c == ' ' || c == '\t';
}

struct internal_repository
{
  repository_location location;
  string display_name;
  dir_path local_path;

  path
  packages_path () const {return local_path / path ("packages");}

  path
  repositories_path () const {return local_path / path ("repositories");}
};

using internal_repositories = vector<internal_repository>;

static internal_repositories
load_repositories (path p)
{
  internal_repositories repos;

  if (p.relative ())
    p.complete ();

  ifstream ifs (p.string ());
  if (!ifs.is_open ())
    throw ifstream::failure (p.string () + ": unable to open");

  ifs.exceptions (ifstream::badbit);

  try
  {
    string s;
    for (uint64_t l (1); getline (ifs, s); ++l)
    {
      auto b (s.cbegin ());
      auto i (b);
      auto e (s.cend ());

      // Skip until first non-space (true) or space (false).
      //
      auto skip = [&i, &e](bool s = true) -> decltype (i)
      {
        for (; i != e && space (*i) == s; ++i)
          ;
        return i;
      };

      skip (); // Skip leading spaces.

      if (i == e || *i == '#') // Empty line or comment.
        continue;

      // From now on pb will track the begining of the next part
      // while i -- the end.
      //
      auto pb (i);  // Location begin.
      skip (false); // Find end of location.

      auto bad_line ([&p, l, &pb, &b](const string& d) {
          ostringstream os;
          os << p << ':' << l << ':' << pb - b + 1 << ": error: " << d;
          throw runtime_error (os.str ());
        });

      repository_location location;

      try
      {
        location = repository_location (string (pb, i));
      }
      catch (const invalid_argument& e)
      {
        bad_line (e.what ());
      }

      if (location.local ())
        bad_line ("local repository location");

      for (const auto& r: repos)
        if (r.location.canonical_name () == location.canonical_name ())
          bad_line ("duplicate canonical name");

      pb = skip (); // Find begin of display name.

      if (pb == e)
        bad_line ("no display name found");

      skip (false); // Find end of display name.

      string name (pb, i);
      pb = skip (); // Find begin of filesystem path.

      if (pb == e) // For now filesystem path is mandatory.
        bad_line ("no filesystem path found");

      skip (false); // Find end of filesystem path (no spaces allowed).

      internal_repository r {
        move (location),
        move (name),
        dir_path (string (pb, i))};

      // If the internal repository local path is relative, then
      // calculate its absolute local path. Such path is considered to be
      // relative to configuration file directory path so result is
      // independent from whichever directory is current for the loader
      // process.
      //
      if (r.local_path.relative ())
        r.local_path = p.directory () / r.local_path;

      try
      {
        r.local_path.normalize ();
      }
      catch (const invalid_path&)
      {
        bad_line ("can't normalize local path");
      }

      if (!file_exists (r.packages_path ()))
        bad_line ("'packages' file does not exist");

      if (!file_exists (r.repositories_path ()))
        bad_line ("'repositories' file does not exist");

      repos.emplace_back (move (r));

      // Check that there is no non-whitespace junk at the end.
      //
      if (skip () != e)
        bad_line ("junk after filesystem path");
    }
  }
  catch (const ifstream::failure&)
  {
    throw ifstream::failure (p.string () + ": io failure");
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
        r.display_name != pr->display_name || r.local_path != pr->local_path ||
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
      query::internal && !query::name.in_range (names.begin (), names.end ())).
    empty ();
}

static timestamp
manifest_stream (const path& p, ifstream& f)
{
  f.open (p.string ());
  if (!f.is_open ())
    throw ifstream::failure (p.string () + ": unable to open");

  f.exceptions (ifstream::badbit | ifstream::failbit);
  return file_mtime (p);
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
  assert (!rp->local_path.empty ());

  package_manifests pkm;

  {
    ifstream ifs;
    path p (rp->local_path / path ("packages"));
    rp->packages_timestamp = manifest_stream (p, ifs);

    manifest_parser mp (ifs, p.string ());
    pkm = package_manifests (mp);
  }

  for (auto& pm: pkm)
  {
    shared_ptr<package> p (db.find<package> (package_id (pm.name, pm.version)));

    if (p == nullptr)
    {
      if (rp->internal)
      {
        // Create internal package object.
        //
        optional<string> dsc;
        if (pm.description)
        {
          if (pm.description->file)
          {
            // @@ Pull description from the file when package manager API
            // is ready.
          }
          else
            dsc = move (*pm.description);
        }

        string chn;
        for (auto& c: pm.changes)
        {
          if (c.file)
          {
            // @@ Pull change notes from the file when package manager
            //    API is ready.
          }
          else
          {
            if (chn.empty ())
              chn = move (c);
            else
              chn += "\n" + c;
          }
        }

        dependencies ds;

        for (auto& pda: pm.dependencies)
        {
          ds.emplace_back (pda.conditional, move (pda.comment));

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
          move (ds),
          move (pm.requirements),
          move (pm.location),
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
      // @@ Need to ensure that the same packages coming from different
      //    repositories are equal. Probably will invent hashsum at some point
      //    for this purpose.
      //

      // As soon as internal repositories get loaded first, the internal
      // package can duplicate an internal package only.
      //
      assert (!rp->internal || p->internal ());

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
  assert (!rp->local_path.empty ());

  // Repository is already persisted by the load_packages() function call.
  //
  assert (db.find<repository> (rp->name) != nullptr);

  repository_manifests rpm;

  {
    ifstream ifs;
    path p (rp->local_path / path ("repositories"));
    rp->repositories_timestamp = manifest_stream (p, ifs);

    manifest_parser mp (ifs, p.string ());
    rpm = repository_manifests (mp);
  }

  for (auto& rm: rpm)
  {
    if (rm.effective_role () == repository_role::prerequisite && !rp->internal)
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
      }

      continue;
    }

    // Load prerequisite or complement repository.
    //
    assert (!rm.location.empty ());

    repository_location rl;

    auto bad_location (
      [&rp, &rm]()
      {
        ostringstream o;
        o << "invalid location '" << rm.location.string ()
          << "' of the prerequisite repository for internal "
          "repository '" << rp->location.string () << "'";

        throw runtime_error (o.str ());
      });

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
    // calculate its absolute local path.
    //
    if (rm.location.relative ())
    {
      dir_path& lp (pr->local_path);
      lp = rp->local_path / rm.location.path ();

      try
      {
        lp.normalize ();
      }
      catch (const invalid_path&)
      {
        ostringstream o;
        o << "can't normalize local path'" << lp.string ()
          << "' of the prerequisite repository for internal "
          "repository '" << rp->location.string () << "'";

        throw runtime_error (o.str ());
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
  if (r == p.internal_repository || find (o.begin (), o.end (), r) != o.end ())
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

        if (c.min_version && c.max_version && *c.min_version == *c.max_version)
        {
          const version& v (*c.min_version);
          q = q && compare_version_eq (vm, v, v.revision != 0);
        }
        else
        {
          if (c.min_version)
          {
            const version& v (*c.min_version);

            if (c.min_open)
              q = q && compare_version_gt (vm, v, v.revision != 0);
            else
              q = q && compare_version_ge (vm, v, v.revision != 0);
          }

          if (c.max_version)
          {
            const version& v (*c.max_version);

            if (c.max_open)
              q = q && compare_version_lt (vm, v, v.revision != 0);
            else
              q = q && compare_version_le (vm, v, v.revision != 0);
          }
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
        ostringstream o;
        o << "can't resolve dependency " << d << " of the package "
          << p.id.name << " " << p.version.string ()
          << " (" << p.internal_repository.load ()->name << ")";

        // Practically it is enough to resolve at least one dependency
        // alternative to build a package. Meanwhile here we consider an error
        // specifying in the manifest file an alternative which can't be
        // resolved.
        //
        throw runtime_error (o.str ());
      }
    }
  }

  db.update (p); // Update the package state.
}

using package_ids = vector<package_id>;

// Ensure the package dependency chain do not contain the package id. Throw
// runtime_error otherwise. Continue the chain with the package id and call
// itself recursively for each prerequisite of the package. Should be called
// once per internal package.
//
// @@ This should probably be eventually moved to bpkg.
//
static void
detect_dependency_cycle (const package_id& id, package_ids& chain, database& db)
{
  // Package of one version depending on the same package of another version
  // is something obscure. So the comparison is made up to a package name.
  //
  auto pr ([&id](const package_id& i) -> bool {return i.name == id.name;});
  auto i (find_if (chain.begin (), chain.end (), pr));

  if (i != chain.end ())
  {
    ostringstream o;
    o << "package dependency cycle: ";

    auto prn (
      [&o, &db](const package_id& id)
      {
        shared_ptr<package> p (db.load<package> (id));
        assert (p->internal () || !p->other_repositories.empty ());

        shared_ptr<repository> r (
          p->internal ()
          ? p->internal_repository.load ()
          : p->other_repositories[0].load ());

        o << id.name << " " << p->version.string () << " (" << r->name << ")";
      });

    for (; i != chain.end (); ++i)
    {
      prn (*i);
      o << " -> ";
    }

    prn (id);
    throw runtime_error (o.str ());
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

int
main (int argc, char* argv[])
try
{
  cli::argv_scanner scan (argc, argv, true);
  options ops (scan);

  // Version.
  //
  if (ops.version ())
  {
    cout << "brep-load " << BREP_VERSION_STR << endl
         << "libbrep " << LIBBREP_VERSION_STR << endl
         << "libbpkg " << LIBBPKG_VERSION_STR << endl
         << "libbutl " << LIBBUTL_VERSION_STR << endl
         << "Copyright (c) 2014-2016 Code Synthesis Ltd" << endl
         << "MIT; see accompanying LICENSE file" << endl;

    return 0;
  }

  // Help.
  //
  if (ops.help ())
  {
    usage (cout);
    return 0;
  }

  if (argc < 2)
  {
    cerr << "<file> argument not provided" << endl;
    usage (cerr);
    return 1;
  }

  if (argc > 2)
  {
    cerr << "unexpected argument encountered" << endl;
    usage (cerr);
    return 1;
  }

  odb::pgsql::database db (ops.db_user (),
                           ops.db_password (),
                           ops.db_name (),
                           ops.db_host (),
                           ops.db_port ());

  // Prevent several brep-load/migrate instances from updating DB
  // simultaneously.
  //
  database_lock l (db);

  transaction t (db.begin ());

  // Check that the database schema matches the current one.
  //
  if (schema_catalog::current_version (db) != db.schema_version ())
  {
    cerr << "database schema differs from the current one" << endl;
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
    // packages.
    //
    uint16_t priority (1);
    for (const auto& ir: irs)
    {
      shared_ptr<repository> r (
        make_shared<repository> (ir.location,
                                 move (ir.display_name),
                                 move (ir.local_path),
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
}
catch (const database_locked&)
{
  cerr << "brep-load or brep-migrate instance is running" << endl;
  return 2;
}
catch (const cli::exception& e)
{
  cerr << e << endl;
  usage (cerr);
  return 1;
}
// Fully qualified to avoid ambiguity with odb exception.
//
catch (const std::exception& e)
{
  cerr << e.what () << endl;
  return 1;
}