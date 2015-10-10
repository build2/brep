// file      : loader/loader.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <vector>
#include <memory>    // shared_ptr, make_shared()
#include <string>
#include <utility>   // move()
#include <cstdint>   // uint64_t
#include <sstream>
#include <cassert>
#include <fstream>
#include <iostream>
#include <stdexcept> // runtime_error, invalid_argument

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
    db.query<repository> (
      query::internal &&
      !query::id.canonical_name.in_range (names.begin (), names.end ())));

  return !rs.empty ();
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
  using brep::optional; // Ambiguity with butl::optional.

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
    shared_ptr<package> p (
      db.find<package> (
        package_id
        {
          pm.name,
          {
            pm.version.epoch,
            pm.version.canonical_upstream,
            pm.version.revision
          }
        }));

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
          move (pm.dependencies),
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

      if (rp->internal)
      {
        // Just skip the duplicate.
        //

        // As soon as internal repositories get loaded first, the internal
        // package can duplicate an internal package only.
        //
        assert (p->internal_repository != nullptr);
      }
      else
      {
        p->external_repositories.push_back (rp);
        db.update (p);
      }
    }
  }

  db.persist (rp); // Save the repository state.
}

// Load the prerequsite repositories state from the 'repositories' file.
// Update the repository persistent state to save repositories_timestamp
// member. Should be called once per internal repository.
//
static void
load_prerequisites (const shared_ptr<repository>& rp, database& db)
{
  // repositories_timestamp other than timestamp_nonexistent signals that
  // repository prerequisites are already loaded.
  //
  assert (rp->repositories_timestamp == timestamp_nonexistent);

  // Load prerequisites for internal repositories only.
  //
  assert (rp->internal);

  // Only locally accessible repositories allowed until package manager API is
  // ready.
  //
  assert (!rp->local_path.empty ());

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
  }

  // Updates repositories_timestamp member.
  //
  db.update (rp);
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
      db.erase_query<package> ();
      db.erase_query<repository> ();

      // On the first pass over the internal repositories we load their
      // packages.
      //
      for (const auto& ir: irs)
      {
        shared_ptr<repository> r (
          make_shared<repository> (ir.location,
                                   move (ir.display_name),
                                   move (ir.local_path)));

        load_packages (r, db);
      }

      // On the second pass over the internal repositories we
      // load their (not yet loaded) prerequisite repositories.
      //
      for (const auto& ir: irs)
      {
        shared_ptr<repository> r (
          db.load<repository> (ir.location.canonical_name ()));

        load_prerequisites (r, db);
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
