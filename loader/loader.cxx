// file      : loader/loader.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <vector>
#include <memory>    // shared_ptr, make_shared()
#include <string>
#include <utility>   // move()
#include <cstdint>   // uint64_t
#include <sstream>
#include <fstream>
#include <iostream>
#include <stdexcept> // runtime_error, invalid_argument

#include <odb/session.hxx>
#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <odb/pgsql/database.hxx>
#include <odb/pgsql/exceptions.hxx>
#include <odb/pgsql/connection.hxx>
#include <odb/pgsql/transaction.hxx>

#include <butl/timestamp>  // timestamp_nonexistent
#include <butl/filesystem>

#include <bpkg/manifest-parser> // manifest_parsing

#include <brep/package>
#include <brep/package-odb>

#include <loader/options>

using namespace std;
using namespace odb::core;
using namespace butl;
using namespace bpkg;
using namespace brep;

namespace pgsql = odb::pgsql;

static void
usage ()
{
  cout << "Usage: brep-loader [options] <file>" << endl
       << "File lists internal repositories." << endl
       << "Options:" << endl;

  options::print_usage (cout);
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
      auto skip ([&i, &e](bool s = true) -> decltype (i) {
          for (; i != e && space (*i) == s; ++i); return i;});

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
      {
        r.local_path = p.directory () / r.local_path;
      }

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
  auto rs (
    db.query<repository> (query::internal &&
                          !query::id.canonical_name.in_range (names.begin (),
                                                              names.end ())));

  return !rs.empty ();
}

// Load the repository state (including of its prerequsite repositories)
// from the 'packages' file.
//
static void
load_repository (const shared_ptr<repository>& rp, database& db)
{
  if (rp->packages_timestamp != timestamp_nonexistent)
    return; // The repository is already loaded.

  // Only locally accessible repositories allowed until package manager API is
  // ready.
  //
  assert (!rp->local_path.empty ());

  auto mstream ([](const path& p, ifstream& f) -> timestamp
    {
      f.open (p.string ());
      if (!f.is_open ())
        throw ifstream::failure (p.string () + ": unable to open");
      f.exceptions (ifstream::badbit | ifstream::failbit);
      return file_mtime (p);
    });

  // Don't add prerequisite repositories for external repositories.
  //
  if (rp->internal)
  {
    repository_manifests rpm;

    {
      ifstream ifs;
      path p (rp->local_path / path ("repositories"));
      rp->repositories_timestamp = mstream (p, ifs);

      manifest_parser mp (ifs, p.string ());
      rpm = repository_manifests (mp);
    }

    for (auto& rm: rpm)
    {
      if (rm.location.empty ())
        continue; // Ignore entry for this repository.

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

      shared_ptr<repository> pr (db.find<repository> (rl.canonical_name ()));

      if (pr == nullptr)
      {
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

        db.persist (pr);
      }

      load_repository (pr, db);

      rp->prerequisite_repositories.emplace_back (pr);
    }
  }

  // Temporary reset ODB session for the current thread while persisting
  // package and package_version objects to decrease memory consumption.
  //
  session& s (session::current ());
  session::reset_current ();

  package_manifests pkm;

  {
    ifstream ifs;
    path p (rp->local_path / path ("packages"));

    // Mark as loaded. This is important in case we try to load this
    // repository again recursively.
    //
    rp->packages_timestamp = mstream (p, ifs);

    manifest_parser mp (ifs, p.string ());
    pkm = package_manifests (mp);
  }

  for (auto& pm: pkm)
  {
    max_package_version mv;

    // If there are no package_version objects persisted yet for this
    // package, then query_one() will leave mv unchanged in which case
    // the version member remains empty. The empty version value is
    // less than any non-empty one so the condition below evaluates
    // to true and the package object gets persisted.
    //
    db.query_one<max_package_version> (
      query<max_package_version>::id.data.package == pm.name, mv);

    if (mv.version < pm.version)
    {
      // Create the package object.
      //
      brep::optional<string> desc; // Ambiguity with butl::optional.

      // Don't add description for external repository packages.
      //
      if (rp->internal && pm.description)
      {
        if (pm.description->file)
        {
          // @@ Pull description from the file when package manager API
          // is ready.
        }
        else
          desc = move (*pm.description);
      }

      package p (pm.name,
                 move (pm.summary),
                 move (pm.tags),
                 move (desc),
                 move (pm.url),
                 move (pm.package_url),
                 move (pm.email),
                 move (pm.package_email));

      if (mv.version.empty ())
        db.persist (p);
      else
        db.update (p);
    }

    // Create package version object.
    //
    dependencies dep;
    requirements req;
    brep::optional<path> loc; // Ambiguity with butl::optional.
    string chn;

    // Don't add dependencies, requirements and changes for external
    // repository packages.
    //
    if (rp->internal)
    {
      dep = move (pm.dependencies);
      req = move (pm.requirements);
      loc = move (pm.location);

      for (auto& c: pm.changes)
      {
        if (c.file)
        {
          // @@ Pull change notes from the file when package manager
          // API is ready.
        }
        else
        {
          if (chn.empty ())
            chn = move (c);
          else
            chn += "\n" + c;
        }
      }
    }

    package_version pv (rp,
                        lazy_shared_ptr<package> (db, pm.name),
                        move (pm.version),
                        pm.priority ? move (*pm.priority) : priority (),
                        move (pm.license_alternatives),
                        move (chn),
                        move (dep),
                        move (req),
                        move (loc));

    db.persist (pv);
  }

  session::current (s); // Restore current session.

  db.update (rp); // Save the repository state.
}

int
main (int argc, char* argv[])
{
  try
  {
    cli::argv_scanner scan (argc, argv, true);
    options ops (scan);

    // Version.
    //
    if (ops.version ())
    {
      cout << "brep-loader 0.0.0" << endl
           << "Copyright (c) 2014-2015 Code Synthesis Ltd" << endl
           << "MIT; see accompanying LICENSE file" << endl;

      return 0;
    }

    // Help.
    //
    if (ops.help ())
    {
      usage ();
      return 0;
    }

    if (argc < 2)
    {
      cout << "<file> argument not provided" << endl;
      usage ();
      return 1;
    }

    if (argc > 2)
    {
      cout << "unexpected argument encountered" << endl;
      usage ();
      return 1;
    }

    pgsql::database db ("", "", "brep", ops.db_host (), ops.db_port ());

    // Prevent several loader instances from updating DB simultaneously.
    //
    {
      transaction t (db.begin ());
      db.execute ("CREATE TABLE IF NOT EXISTS loader_mutex ()");
      t.commit ();
    }

    pgsql::connection_ptr synch_c (db.connection ());

    // Don't make current.
    //
    pgsql::transaction synch_t (synch_c->begin (), false);

    try
    {
      synch_c->execute ("LOCK TABLE loader_mutex NOWAIT");
    }
    catch (const pgsql::database_exception& e)
    {
      if (e.sqlstate () == "55P03")
        return 2; // Other loader instance acquired the mutex.

      throw;
    }

    // Load the description of all the internal repositories from
    // the configuration file.
    //
    internal_repositories irs (load_repositories (path (argv[1])));

    transaction t (db.begin ());

    if (changed (irs, db))
    {
      // Rebuild repositories persistent state from scratch.
      //
      db.erase_query<repository> ();
      db.erase_query<package> ();
      db.erase_query<package_version> ();

      // We use repository object packages_timestamp as a flag to signal that
      // we have already loaded this repo. The easiest way to make
      // it work in case of cycles is to use a session. This way,
      // the repository object on which we updated the packages_timestamp
      // will be the same as the one we may check down the call
      // stack.
      //
      session s;

      // On the first pass over the internal repositories list we
      // persist empty repository objects, setting the interal flag
      // to true and packages_timestamp to non-existent. The idea is to
      // establish the "final" list of internal repositories.
      //
      for (auto& ir: irs)
      {
        shared_ptr<repository> r (
          make_shared<repository> (ir.location,
                                   move (ir.display_name),
                                   move (ir.local_path)));

        db.persist (r);
      }

      // On the second pass over the internal repositories we
      // load them and all their (not yet loaded) prerequisite
      // repositories.
      //
      for (const auto& ir: irs)
      {
        shared_ptr<repository> r (
          db.load<repository> (ir.location.canonical_name ()));

        load_repository (r, db);
      }
    }

    t.commit ();
    synch_t.commit (); // Release the mutex.
  }
  catch (const cli::exception& e)
  {
    cerr << e << endl;
    usage ();
    return 1;
  }
  // Fully qualified to avoid ambiguity with odb exception.
  //
  catch (const std::exception& e)
  {
    cerr << e.what () << endl;
    return 1;
  }
}
