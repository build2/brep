// file      : tests/load/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <iostream>
#include <exception>
#include <algorithm> // sort(), find()

#include <odb/session.hxx>
#include <odb/transaction.hxx>

#include <odb/pgsql/database.hxx>

#include <libbutl/process.mxx>
#include <libbutl/filesystem.mxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <libbrep/package.hxx>
#include <libbrep/package-odb.hxx>

using namespace std;
using namespace odb::core;
using namespace butl;
using namespace brep;

static bool
check_location (shared_ptr<package>& p)
{
  if (p->internal ())
    return p->location && *p->location ==
      path (p->id.name + "-" + p->version.string () + ".tar.gz");
  else
    return !p->location;
}

static bool
check_external (const package& p)
{
  return p.summary.empty () && p.tags.empty () && !p.description &&
    p.url.empty () && !p.package_url && p.email.empty () &&
    !p.package_email && !p.internal () && p.other_repositories.size () > 0 &&
    p.priority == priority () && p.changes.empty () &&
    p.license_alternatives.empty () && p.dependencies.empty () &&
    p.requirements.empty () && !p.sha256sum;
}

namespace bpkg
{
  static bool
  operator== (const build_constraint& x, const build_constraint& y)
  {
    return x.exclusion == y.exclusion && x.config == y.config &&
      x.target == y.target && x.comment == y.comment;
  }
}

int
main (int argc, char* argv[])
{
  auto print_usage = [argv]()
  {
    cerr << "usage: " << argv[0]
         << " <loader_path> [loader_options] <loadtab_file>" << endl;
  };

  if (argc < 3)
  {
    print_usage ();
    return 1;
  }

  string user;
  string password;
  string name ("brep_package");
  string host;
  unsigned int port (0);
  int i (2);

  for (; i < argc - 1; ++i)
  {
    string n (argv[i]);
    if (n == "--db-user" || n == "-u")
      user = argv[++i];
    else if (n == "--db-password")
      password = argv[++i];
    else if (n == "--db-name" || n == "-n")
      name = argv[++i];
    else if (n == "--db-host" || n == "-h")
      host = argv[++i];
    else if (n == "--db-port" || n == "-p")
      port = stoul (argv[++i]);
  }

  if (i != argc - 1)
  {
    print_usage ();
    return 1;
  }

  try
  {
    path cp (argv[argc - 1]);

    // Make configuration file path absolute to use it's directory as base for
    // internal repositories relative local paths.
    //
    if (cp.relative ())
      cp.complete ();

    // Update packages file timestamp to enforce loader to update
    // persistent state.
    //
    path p (cp.directory () / path ("1/stable/packages"));
    char const* args[] = {"touch", p.string ().c_str (), nullptr};
    assert (process (args).wait ());

    timestamp srt (file_mtime (p));

    // Run the loader.
    //
    char const** ld_args (const_cast<char const**> (argv + 1));
    assert (process (ld_args).wait ());

    // Check persistent objects validity.
    //
    odb::pgsql::database db (
      user,
      password,
      name,
      host,
      port,
      "options='-c default_transaction_isolation=serializable'");

    {
      session s;
      transaction t (db.begin ());

      assert (db.query<repository> ().size () == 7);
      assert (db.query<package> ().size () == 17);

      shared_ptr<repository> sr (
        db.load<repository> ("dev.cppget.org/stable"));

      shared_ptr<repository> mr (
        db.load<repository> ("dev.cppget.org/math"));

      shared_ptr<repository> cr (
        db.load<repository> ("dev.cppget.org/misc"));

      shared_ptr<repository> tr (
        db.load<repository> ("dev.cppget.org/testing"));

      shared_ptr<repository> gr (
        db.load<repository> ("dev.cppget.org/staging"));

      // Verify 'stable' repository.
      //
      assert (sr->location.canonical_name () == "dev.cppget.org/stable");
      assert (sr->location.string () ==
              "http://dev.cppget.org/1/stable");
      assert (sr->display_name == "stable");
      assert (sr->priority == 1);
      assert (!sr->url);
      assert (sr->email && *sr->email == "repoman@dev.cppget.org" &&
              sr->email->comment == "public mailing list");
      assert (sr->summary &&
              *sr->summary == "General C++ package stable repository");
      assert (sr->description && *sr->description ==
              "This is the awesome C++ package repository full of exciting "
              "stuff.");

      dir_path srp (cp.directory () / dir_path ("1/stable"));
      assert (sr->cache_location.path () == srp.normalize ());

      assert (sr->packages_timestamp == srt);
      assert (sr->repositories_timestamp ==
              file_mtime (
                dir_path (
                  sr->cache_location.path ()) / path ("repositories")));

      assert (sr->internal);
      assert (sr->complements.empty ());
      assert (sr->prerequisites.size () == 2);
      assert (sr->prerequisites[0].load () == cr);
      assert (sr->prerequisites[1].load () == mr);

      // Verify libfoo package versions.
      //
      // libfoo-1.0
      //
      shared_ptr<package> fpv1 (
        db.load<package> (package_id ("libfoo", version ("1.0"))));

      assert (fpv1->summary == "The Foo Library");
      assert (fpv1->tags.empty ());
      assert (!fpv1->description);
      assert (fpv1->url == "http://www.example.com/foo/");
      assert (!fpv1->package_url);
      assert (fpv1->email == "foo-users@example.com");
      assert (!fpv1->package_email);

      assert (fpv1->internal_repository.load () == sr);
      assert (fpv1->other_repositories.size () == 2);
      assert (fpv1->other_repositories[0].load () == mr);
      assert (fpv1->other_repositories[1].load () == cr);

      assert (fpv1->priority == priority::low);
      assert (fpv1->changes.empty ());

      assert (fpv1->license_alternatives.size () == 1);
      assert (fpv1->license_alternatives[0].size () == 1);
      assert (fpv1->license_alternatives[0][0] == "MIT");

      assert (fpv1->dependencies.empty ());
      assert (fpv1->requirements.empty ());

      assert (check_location (fpv1));

      assert (fpv1->sha256sum && *fpv1->sha256sum ==
        "d8ad319b55fdd19ff24cb0fcf9d61101289569f80b8688884389587cfafa1f1e");

      // libfoo-1.2.2
      //
      shared_ptr<package> fpv2 (
        db.load<package> (package_id ("libfoo", version ("1.2.2"))));

      assert (fpv2->summary == "The Foo library");
      assert (fpv2->tags == strings ({"c++", "foo"}));
      assert (!fpv2->description);
      assert (fpv2->url == "http://www.example.com/foo/");
      assert (!fpv2->package_url);
      assert (fpv2->email == "foo-users@example.com");
      assert (!fpv2->package_email);

      assert (fpv2->internal_repository.load () == sr);
      assert (fpv2->other_repositories.empty ());
      assert (fpv2->priority == priority::low);
      assert (fpv2->changes.empty ());

      assert (fpv2->license_alternatives.size () == 1);
      assert (fpv2->license_alternatives[0].size () == 1);
      assert (fpv2->license_alternatives[0][0] == "MIT");

      assert (fpv2->dependencies.size () == 2);
      assert (fpv2->dependencies[0].size () == 1);
      assert (fpv2->dependencies[1].size () == 1);

      auto dep = [&db] (
        const char* n, const optional<dependency_constraint>& c) -> dependency
      {
        return {lazy_shared_ptr<package> (db, package_id (n, version ())), c};
      };

      assert (fpv2->dependencies[0][0] ==
              dep (
                "libbar",
                optional<dependency_constraint> (
                  dependency_constraint (
                    nullopt, true, version ("2.4.0"), false))));

      assert (fpv2->dependencies[1][0] ==
              dep (
                "libexp",
                optional<dependency_constraint> (
                  dependency_constraint (
                    version ("1~1.2"), false, version ("1~1.2"), false))));

      assert (check_location (fpv2));

      assert (fpv2->sha256sum && *fpv2->sha256sum ==
        "b47de1b207ef097c9ecdd560007aeadd3775f4fafb4f96fb983e9685c21f3980");

      // libfoo-1.2.2-alpha.1
      //
      shared_ptr<package> fpv2a (
        db.load<package> (package_id ("libfoo", version ("1.2.2-alpha.1"))));

      assert (fpv2a->summary == "The Foo library");
      assert (fpv2a->tags == strings ({"c++", "foo"}));
      assert (!fpv2a->description);
      assert (fpv2a->url == "http://www.example.com/foo/");
      assert (!fpv2a->package_url);
      assert (fpv2a->email == "foo-users@example.com");
      assert (!fpv2a->package_email);

      assert (fpv2a->internal_repository.load () == sr);
      assert (fpv2a->other_repositories.empty ());
      assert (fpv2a->priority == priority::low);
      assert (fpv2a->changes.empty ());

      assert (fpv2a->license_alternatives.size () == 1);
      assert (fpv2a->license_alternatives[0].size () == 1);
      assert (fpv2a->license_alternatives[0][0] == "MIT");

      assert (fpv2a->dependencies.size () == 3);
      assert (fpv2a->dependencies[0].size () == 2);
      assert (fpv2a->dependencies[1].size () == 1);
      assert (fpv2a->dependencies[2].size () == 2);

      assert (fpv2a->dependencies[0][0] ==
              dep (
                "libmisc",
                optional<dependency_constraint> (
                  dependency_constraint (
                    version ("0.1"), false, version ("2.0.0-"), true))));

      assert (fpv2a->dependencies[0][1] ==
              dep (
                "libmisc",
                optional<dependency_constraint> (
                  dependency_constraint (
                    version ("2.0"), false, version ("5.0"), false))));

      assert (fpv2a->dependencies[1][0] ==
              dep (
                "libgenx",
                optional<dependency_constraint> (
                  dependency_constraint (
                    version ("0.2"), true, version ("3.0"), true))));

      assert (fpv2a->dependencies[2][0] ==
              dep (
                "libexpat",
                optional<dependency_constraint> (
                  dependency_constraint (
                    nullopt, true, version ("5.2"), true))));

      assert (fpv2a->dependencies[2][1] ==
              dep (
                "libexpat",
                optional<dependency_constraint> (
                  dependency_constraint (
                    version ("1"), true, version ("5.1"), false))));

      assert (fpv2a->requirements.empty ());

      assert (check_location (fpv2a));

      assert (fpv2a->sha256sum && *fpv2a->sha256sum ==
        "34fc224087bfd9212de4acfbbf5275513ebc57678b5f029546918a62c57d15cb");

      // libfoo-1.2.3-4
      //
      shared_ptr<package> fpv3 (
        db.load<package> (package_id ("libfoo", version ("1.2.3+4"))));

      assert (fpv3->summary == "The Foo library");
      assert (fpv3->tags == strings ({"c++", "foo"}));
      assert (!fpv3->description);
      assert (fpv3->url == "http://www.example.com/foo/");
      assert (!fpv3->package_url);
      assert (fpv3->email == "foo-users@example.com");
      assert (!fpv3->package_email);

      assert (fpv3->internal_repository.load () == sr);
      assert (fpv3->other_repositories.empty ());
      assert (fpv3->priority == priority::low);

      assert (fpv3->changes.empty ());

      assert (fpv3->license_alternatives.size () == 1);
      assert (fpv3->license_alternatives[0].size () == 1);
      assert (fpv3->license_alternatives[0][0] == "MIT");

      assert (fpv3->dependencies.size () == 1);
      assert (fpv3->dependencies[0].size () == 1);
      assert (fpv3->dependencies[0][0] ==
              dep (
                "libmisc",
                optional<dependency_constraint> (
                  dependency_constraint (
                    version ("2.0.0"), false, nullopt, true))));

      assert (check_location (fpv3));

      assert (fpv3->sha256sum && *fpv3->sha256sum ==
        "204fb25edf2404e9e88e1bef8b2a444281a807d9087093147a2cc80a1ffba79a");

      // libfoo-1.2.4
      //
      shared_ptr<package> fpv4 (
        db.load<package> (package_id ("libfoo", version ("1.2.4"))));

      assert (fpv4->summary == "The Foo Library");
      assert (fpv4->tags == strings ({"c++", "foo"}));
      assert (*fpv4->description == "Very good foo library.");
      assert (fpv4->url == "http://www.example.com/foo/");
      assert (!fpv4->package_url);
      assert (fpv4->email == "foo-users@example.com");
      assert (!fpv4->package_email);

      assert (fpv4->internal_repository.load () == sr);
      assert (fpv4->other_repositories.empty ());
      assert (fpv4->priority == priority::low);
      assert (fpv4->changes == "some changes 1\n\nsome changes 2");

      assert (fpv4->license_alternatives.size () == 1);
      assert (fpv4->license_alternatives[0].comment ==
              "Permissive free software license.");
      assert (fpv4->license_alternatives[0].size () == 1);
      assert (fpv4->license_alternatives[0][0] == "MIT");

      assert (fpv4->dependencies.size () == 1);
      assert (fpv4->dependencies[0].size () == 1);
      assert (fpv4->dependencies[0][0] ==
              dep (
                "libmisc",
                optional<dependency_constraint> (
                  dependency_constraint (
                    version ("2.0.0"), false, nullopt, true))));

      assert (check_location (fpv4));

      assert (fpv4->sha256sum && *fpv4->sha256sum ==
        "aa1606323bfc59b70de642629dc5d8318cc5348e3646f90ed89406d975db1e1d");

      // Verify 'math' repository.
      //
      assert (mr->location.canonical_name () == "dev.cppget.org/math");
      assert (mr->location.string () ==
              "http://dev.cppget.org/1/math");
      assert (mr->display_name == "math");
      assert (mr->priority == 2);
      assert (!mr->url);
      assert (mr->email && *mr->email == "repoman@dev.cppget.org");
      assert (mr->summary && *mr->summary == "Math C++ package repository");
      assert (mr->description && *mr->description ==
              "This is the awesome C++ package repository full of remarkable "
              "algorithms and\nAPIs.");

      dir_path mrp (cp.directory () / dir_path ("1/math"));
      assert (mr->cache_location.path () == mrp.normalize ());

      assert (mr->packages_timestamp ==
              file_mtime (
                dir_path (mr->cache_location.path ()) / path ("packages")));

      assert (mr->repositories_timestamp ==
              file_mtime (
                dir_path (
                  mr->cache_location.path ()) / path ("repositories")));

      assert (mr->internal);

      assert (mr->complements.empty ());
      assert (mr->prerequisites.size () == 1);
      assert (mr->prerequisites[0].load () == cr);

      // Verify libstudxml package version.
      //
      shared_ptr<package> xpv (
        db.load<package> (package_id ("libstudxml", version ("1.0.0+1"))));

      assert (xpv->summary == "Modern C++ XML API");
      assert (xpv->tags == strings ({"c++", "xml", "parser", "serializer",
              "pull", "streaming", "modern"}));
      assert (!xpv->description);
      assert (xpv->url == "http://www.codesynthesis.com/projects/libstudxml/");
      assert (!xpv->package_url);
      assert (xpv->email ==
              email ("studxml-users@codesynthesis.com",
                     "Public mailing list, posts by  non-members "
                     "are allowed but moderated."));
      assert (xpv->package_email &&
              *xpv->package_email == email ("boris@codesynthesis.com",
                                            "Direct email to the author."));

      assert (xpv->internal_repository.load () == mr);
      assert (xpv->other_repositories.empty ());
      assert (xpv->priority == priority::low);
      assert (xpv->changes.empty ());

      assert (xpv->license_alternatives.size () == 1);
      assert (xpv->license_alternatives[0].size () == 1);
      assert (xpv->license_alternatives[0][0] == "MIT");

      assert (xpv->dependencies.size () == 2);
      assert (xpv->dependencies[0].size () == 1);
      assert (xpv->dependencies[0][0] ==
              dep (
                "libexpat",
                optional<dependency_constraint> (
                  dependency_constraint (
                    version ("2.0.0"), false, nullopt, true))));

      assert (xpv->dependencies[1].size () == 1);
      assert (xpv->dependencies[1][0] == dep ("libgenx", nullopt));

      assert (xpv->requirements.empty ());

      assert (check_location (xpv));

      assert (xpv->sha256sum && *xpv->sha256sum ==
        "cfa4b1f89f8e903d48eff1e1d14628c32aa4d126d09b0b056d2cd80f8dc78580");

      // Verify libfoo package versions.
      //
      // libfoo-1.2.4-1
      //
      shared_ptr<package> fpv5 (
        db.load<package> (package_id ("libfoo", version ("1.2.4+1"))));

      assert (fpv5->summary == "The Foo Math Library");
      assert (fpv5->tags == strings ({"c++", "foo", "math"}));
      assert (*fpv5->description ==
              "A modern C++ library with easy to use linear algebra and lot "
              "of optimization\ntools.\n\nThere are over 100 functions in "
              "total with an extensive test suite. The API is\nsimilar to "
              "MATLAB.\n\nUseful for conversion of research code into "
              "production environments.");

      assert (fpv5->url == "http://www.example.com/foo/");

      assert (fpv5->doc_url && *fpv5->doc_url ==
              "http://www.example.org/projects/libfoo/man.xhtml" &&
              fpv5->doc_url->comment == "Documentation page.");

      assert (fpv5->src_url && *fpv5->src_url ==
              "http://scm.example.com/?p=odb/libodb.git;a=tree" &&
              fpv5->src_url->comment == "Source tree url.");

      assert (fpv5->package_url &&
              *fpv5->package_url == "http://www.example.com/foo/pack");
      assert (fpv5->email == "foo-users@example.com");
      assert (fpv5->package_email &&
              *fpv5->package_email == "pack@example.com");

      assert (fpv5->internal_repository.load () == mr);
      assert (fpv5->other_repositories.size () == 1);
      assert (fpv5->other_repositories[0].load () == cr);

      assert (fpv5->priority == priority::high);
      assert (fpv5->priority.comment ==
              "Critical bug fixes, performance improvement.");

      const char ch[] = R"DLM(1.2.4+1
 * applied patch for critical bug-219
 * regenerated documentation

1.2.4
 * test suite extended significantly)DLM";

      assert (fpv5->changes == ch);

      assert (fpv5->license_alternatives.size () == 2);
      assert (fpv5->license_alternatives[0].comment ==
              "If using with GNU TLS.");
      assert (fpv5->license_alternatives[0].size () == 2);
      assert (fpv5->license_alternatives[0][0] == "LGPLv2");
      assert (fpv5->license_alternatives[0][1] == "MIT");
      assert (fpv5->license_alternatives[1].comment ==
              "If using with OpenSSL.");
      assert (fpv5->license_alternatives[1].size () == 1);
      assert (fpv5->license_alternatives[1][0] == "BSD");

      assert (fpv5->dependencies.size () == 3);
      assert (fpv5->dependencies[0].size () == 2);
      assert (fpv5->dependencies[0].comment ==
              "Crashes with 1.1.0-2.3.0.");

      assert (fpv5->dependencies[0][0] ==
              dep (
                "libmisc",
                optional<dependency_constraint> (
                  dependency_constraint (
                    nullopt, true, version ("1.1"), true))));

      assert (fpv5->dependencies[0][1] ==
              dep (
                "libmisc",
                optional<dependency_constraint> (
                  dependency_constraint (
                    version ("2.3.0"), true, nullopt, true))));

      assert (fpv5->dependencies[1].size () == 1);
      assert (fpv5->dependencies[1].comment.empty ());

      assert (fpv5->dependencies[1][0] ==
              dep ("libexp",
                   optional<dependency_constraint> (
                     dependency_constraint (
                       version ("1.0"), false, nullopt, true))));

      assert (fpv5->dependencies[2].size () == 2);
      assert (fpv5->dependencies[2].comment == "The newer the better.");

      assert (fpv5->dependencies[2][0] == dep ("libstudxml", nullopt));
      assert (fpv5->dependencies[2][1] == dep ("libexpat", nullopt));

      requirements& fpvr5 (fpv5->requirements);
      assert (fpvr5.size () == 4);

      assert (fpvr5[0] == strings ({"linux", "windows", "macosx"}));
      assert (!fpvr5[0].conditional);
      assert (fpvr5[0].comment == "Symbian support is coming.");

      assert (fpvr5[1] == strings ({"c++11"}));
      assert (!fpvr5[1].conditional);
      assert (fpvr5[1].comment.empty ());

      assert (fpvr5[2].empty ());
      assert (fpvr5[2].conditional);
      assert (fpvr5[2].comment ==
              "libc++ standard library if using Clang on Mac OS X.");

      assert (fpvr5[3] == strings ({"vc++ >= 12.0"}));
      assert (fpvr5[3].conditional);
      assert (fpvr5[3].comment == "Only if using VC++ on Windows.");

      assert (check_location (fpv5));

      assert (fpv5->sha256sum && *fpv5->sha256sum ==
        "c5e593d8efdc34a258f8c0b8cc352dc7193ea4a1d666bcf8d48708c7dd82d0d6");

      // Verify libexp package version.
      //
      // libexp-1+1.2
      //
      shared_ptr<package> epv (
        db.load<package> (package_id ("libexp", version ("1~1.2+1"))));

      assert (epv->summary == "The exponent");
      assert (epv->tags == strings ({"c++", "exponent"}));
      assert (epv->description && *epv->description ==
              "The exponent math function.");
      assert (epv->url == "http://www.exp.com");
      assert (!epv->package_url);
      assert (epv->email == email ("users@exp.com"));
      assert (!epv->package_email);
      assert (epv->build_email && *epv->build_email == "builds@exp.com");

      assert (epv->internal_repository.load () == mr);
      assert (epv->other_repositories.empty ());
      assert (epv->priority == priority (priority::low));
      assert (epv->changes.empty ());

      assert (epv->license_alternatives.size () == 1);
      assert (epv->license_alternatives[0].size () == 1);
      assert (epv->license_alternatives[0][0] == "MIT");

      assert (epv->dependencies.size () == 2);
      assert (epv->dependencies[0].size () == 1);
      assert (epv->dependencies[0][0] == dep ("libmisc", nullopt));

      assert (epv->dependencies[1].size () == 1);
      assert (epv->dependencies[1][0] ==
              dep ("libpq",
                   optional<dependency_constraint> (
                     dependency_constraint (
                       version ("9.0.0"), false, nullopt, true))));

      assert (epv->requirements.empty ());

      db.load (*epv, epv->build_section);

      assert (
        epv->build_constraints ==
        build_constraints ({
          build_constraint (true, "*", nullopt, "Only supported on Linux."),
          build_constraint (false, "linux*", nullopt, "")}));

      assert (check_location (epv));
      assert (epv->sha256sum && *epv->sha256sum ==
        "0a7414d06ad26d49dad203deaf3841f3df97f1fe27c5bf190c1c20dfeb7f84e0");

      // Verify libpq package version.
      //
      // libpq-0
      //
      shared_ptr<package> qpv (
        db.load<package> (package_id ("libpq", version ("0"))));

      assert (qpv->summary == "PostgreSQL C API client library");

      // Verify 'misc' repository.
      //
      assert (cr->location.canonical_name () == "dev.cppget.org/misc");
      assert (cr->location.string () ==
              "http://dev.cppget.org/1/misc");
      assert (cr->display_name.empty ());
      assert (cr->priority == 0);
      assert (cr->url && *cr->url == "http://misc.cppget.org/");
      assert (!cr->email);
      assert (!cr->summary);
      assert (!cr->description);

      dir_path crp (cp.directory () / dir_path ("1/misc"));
      assert (cr->cache_location.path () == crp.normalize ());

      assert (cr->packages_timestamp ==
              file_mtime (
                dir_path (cr->cache_location.path ()) / path ("packages")));

      assert (cr->repositories_timestamp ==
              file_mtime (
                dir_path (
                  cr->cache_location.path ()) / path ("repositories")));

      assert (!cr->internal);
      assert (cr->prerequisites.empty ());
      assert (cr->complements.size () == 1);
      assert (cr->complements[0].load () == tr);

      // Verify libbar package version.
      //
      // libbar-2.4.0+3
      //
      shared_ptr<package> bpv (
        db.load<package> (package_id ("libbar", version ("2.4.0+3"))));

      assert (check_external (*bpv));
      assert (bpv->other_repositories.size () == 1);
      assert (bpv->other_repositories[0].load () == cr);
      assert (check_location (bpv));

      // Verify libfoo package versions.
      //
      // libfoo-0.1
      //
      shared_ptr<package> fpv0 (
        db.load<package> (package_id ("libfoo", version ("0.1"))));

      assert (check_external (*fpv0));
      assert (fpv0->other_repositories.size () == 1);
      assert (fpv0->other_repositories[0].load () == cr);
      assert (check_location (fpv0));

      // libfoo-1.2.4-2
      //
      shared_ptr<package> fpv6 (
        db.load<package> (package_id ("libfoo", version ("1.2.4+2"))));

      assert (check_external (*fpv6));
      assert (fpv6->other_repositories.size () == 1);
      assert (fpv6->other_repositories[0].load () == cr);
      assert (check_location (fpv6));

      // Verify 'testing' repository.
      //
      assert (tr->location.canonical_name () == "dev.cppget.org/testing");
      assert (tr->location.string () ==
              "http://dev.cppget.org/1/testing");
      assert (tr->display_name.empty ());
      assert (tr->priority == 0);
      assert (tr->url && *tr->url == "http://test.cppget.org/hello/");
      assert (!tr->email);
      assert (!tr->summary);
      assert (!tr->description);

      dir_path trp (cp.directory () / dir_path ("1/testing"));
      assert (tr->cache_location.path () == trp.normalize ());

      assert (tr->packages_timestamp ==
              file_mtime (
                dir_path (tr->cache_location.path ()) / path ("packages")));

      assert (tr->repositories_timestamp ==
              file_mtime (
                dir_path (
                  tr->cache_location.path ()) / path ("repositories")));

      assert (!tr->internal);
      assert (tr->prerequisites.empty ());
      assert (tr->complements.size () == 1);
      assert (tr->complements[0].load () == gr);

      // Verify libmisc package version.
      //
      // libmisc-2.4.0
      //
      shared_ptr<package> mpv0 (
        db.load<package> (package_id ("libmisc", version ("2.4.0"))));

      assert (check_external (*mpv0));
      assert (mpv0->other_repositories.size () == 1);
      assert (mpv0->other_repositories[0].load () == tr);
      assert (check_location (mpv0));

      // libmisc-2.3.0+1
      //
      shared_ptr<package> mpv1 (
        db.load<package> (package_id ("libmisc", version ("2.3.0+1"))));

      assert (check_external (*mpv1));
      assert (mpv1->other_repositories.size () == 1);
      assert (mpv1->other_repositories[0].load () == tr);
      assert (check_location (mpv1));

      // Verify 'staging' repository.
      //
      assert (gr->location.canonical_name () == "dev.cppget.org/staging");
      assert (gr->location.string () ==
              "http://dev.cppget.org/1/staging");
      assert (gr->display_name.empty ());
      assert (gr->priority == 0);
      assert (gr->url && *gr->url == "http://dev.cppget.org/");
      assert (!gr->email);
      assert (!gr->summary);
      assert (!gr->description);

      dir_path grp (cp.directory () / dir_path ("1/staging"));
      assert (gr->cache_location.path () == grp.normalize ());

      assert (gr->packages_timestamp ==
              file_mtime (
                dir_path (gr->cache_location.path ()) / path ("packages")));

      assert (gr->repositories_timestamp ==
              file_mtime (
                dir_path (
                  gr->cache_location.path ()) / path ("repositories")));

      assert (!gr->internal);
      assert (gr->prerequisites.empty ());
      assert (gr->complements.empty ());

      // Verify libexpat package version.
      //
      // libexpat-5.1
      //
      shared_ptr<package> tpv (
        db.load<package> (package_id ("libexpat", version ("5.1"))));

      assert (check_external (*tpv));
      assert (tpv->other_repositories.size () == 1);
      assert (tpv->other_repositories[0].load () == gr);
      assert (check_location (tpv));

      // Verify libgenx package version.
      //
      // libgenx-1.0
      //
      shared_ptr<package> gpv (
        db.load<package> (package_id ("libgenx", version ("1.0"))));

      assert (check_external (*gpv));
      assert (gpv->other_repositories.size () == 1);
      assert (gpv->other_repositories[0].load () == gr);
      assert (check_location (gpv));

      // Verify libmisc package version.
      //
      // libmisc-1.0
      //
      shared_ptr<package> mpv2 (
        db.load<package> (package_id ("libmisc", version ("1.0"))));

      assert (check_external (*mpv2));
      assert (mpv2->other_repositories.size () == 1);
      assert (mpv2->other_repositories[0].load () == gr);
      assert (check_location (mpv2));

      // Change package summary, update the object persistent state, rerun
      // loader and ensure the model were not rebuilt.
      //
      bpv->summary = "test";
      db.update (bpv);

      t.commit ();
    }

    assert (process (ld_args).wait ());
    transaction t (db.begin ());

    shared_ptr<package> bpv (
      db.load<package> (package_id ("libbar", version ("2.4.0+3"))));

    assert (bpv->summary == "test");

    t.commit ();
  }
  // Fully qualified to avoid ambiguity with odb exception.
  //
  catch (const std::exception& e)
  {
    cerr << e << endl;
    return 1;
  }
}
