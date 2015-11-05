// file      : tests/loader/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <vector>
#include <memory>    // shared_ptr
#include <string>
#include <cassert>
#include <iostream>
#include <exception>
#include <algorithm> // sort(), find()

#include <odb/session.hxx>
#include <odb/transaction.hxx>

#include <odb/pgsql/database.hxx>

#include <butl/process>
#include <butl/timestamp>  // timestamp_nonexistent
#include <butl/filesystem>

#include <brep/package>
#include <brep/package-odb>

using namespace std;
using namespace odb::core;
using namespace butl;
using namespace brep;

static inline bool
operator== (const dependency& a, const dependency& b)
{
  return a.name () == b.name () && !a.constraint == !b.constraint &&
    (!a.constraint || (a.constraint->operation == b.constraint->operation &&
                      a.constraint->version == b.constraint->version));
}

static bool
check_location (shared_ptr<package>& p)
{
  if (p->internal_repository == nullptr)
    return !p->location;
  else
    return p->location && *p->location ==
      path (p->id.name + "-" + p->version.string () + ".tar.gz");
}

static bool
check_external (const package& p)
{
  return p.summary.empty () && p.tags.empty () && !p.description &&
    p.url.empty () && !p.package_url && p.email.empty () && !p.package_email &&
    p.internal_repository == nullptr && p.other_repositories.size () > 0 &&
    p.priority == priority () && p.changes.empty () &&
    p.license_alternatives.empty () && p.dependencies.empty () &&
    p.requirements.empty ();
}

int
main (int argc, char* argv[])
{
  using brep::optional; // Ambiguity with butl::optional.

  if (argc != 7)
  {
    cerr << "usage: " << argv[0]
         << " <loader_path> --db-host <host> --db-port <port>"
         << " <loader_conf_file>" << endl;

    return 1;
  }

  try
  {
    path cp (argv[6]);

    // Make configuration file path absolute to use it's directory as base for
    // internal repositories relative local paths.
    //
    if (cp.relative ())
      cp.complete ();

    // Update packages file timestamp to enforce loader to update
    // persistent state.
    //
    path p (cp.directory () / path ("internal/1/stable/packages"));
    char const* args[] = {"touch", p.string ().c_str (), nullptr};
    assert (process (args).wait ());

    timestamp srt (file_mtime (p));

    // Run the loader.
    //
    char const** ld_args (const_cast<char const**> (argv + 1));
    assert (process (ld_args).wait ());

    // Check persistent objects validity.
    //
    odb::pgsql::database db ("", "", "brep", argv[3], stoul (argv[5]));

    {
      session s;
      transaction t (db.begin ());

      assert (db.query<repository> ().size () == 5);
      assert (db.query<package> ().size () == 14);

      shared_ptr<repository> sr (db.load<repository> ("cppget.org/stable"));
      shared_ptr<repository> mr (db.load<repository> ("cppget.org/math"));
      shared_ptr<repository> cr (db.load<repository> ("cppget.org/misc"));
      shared_ptr<repository> tr (db.load<repository> ("cppget.org/testing"));
      shared_ptr<repository> gr (db.load<repository> ("cppget.org/staging"));

      // Verify 'stable' repository.
      //
      assert (sr->location.canonical_name () == "cppget.org/stable");
      assert (sr->location.string () ==
              "http://pkg.cppget.org/internal/1/stable");
      assert (sr->display_name == "stable");

      dir_path srp (cp.directory () / dir_path ("internal/1/stable"));
      assert (sr->local_path == srp.normalize ());

      assert (sr->packages_timestamp == srt);
      assert (sr->repositories_timestamp ==
              file_mtime (dir_path (sr->local_path) / path ("repositories")));
      assert (sr->internal);

      shared_ptr<package> fpv1 (
        db.load<package> (package_id ("libfoo", version ("1.0"))));
      assert (check_location (fpv1));

      shared_ptr<package> fpv2 (
        db.load<package> (package_id ("libfoo", version ("1.2.2"))));
      assert (check_location (fpv2));

      shared_ptr<package> fpv3 (
        db.load<package> (package_id ("libfoo", version ("1.2.3-4"))));
      assert (check_location (fpv3));

      shared_ptr<package> fpv4 (
        db.load<package> (package_id ("libfoo", version ("1.2.4"))));
      assert (check_location (fpv4));

      assert (sr->complements.empty ());
      assert (sr->prerequisites.size () == 2);
      assert (sr->prerequisites[0].load () == cr);
      assert (sr->prerequisites[1].load () == mr);

      // Verify libfoo package versions.
      //
      // libfoo-1.0
      //
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

      // libfoo-1.2.2
      //
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

      auto dep (
        [&db](const char* n,
              const optional<dependency_constraint>& c) -> dependency
        {
          return {lazy_shared_ptr<package> (db, package_id (n, version ())), c};
        });

      assert (fpv2->dependencies[0][0] ==
              dep (
                "libbar",
                optional<dependency_constraint> (
                  dependency_constraint{comparison::le, version ("2.4.0")})));

      assert (fpv2->dependencies[1][0] ==
              dep (
                "libexp",
                brep::optional<dependency_constraint> (
                  dependency_constraint{comparison::eq, version ("1+1.2")})));

      assert (fpv2->requirements.empty ());

      // libfoo-1.2.3-4
      //
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
                brep::optional<dependency_constraint> (
                  dependency_constraint{comparison::ge, version ("2.0.0")})));

      // libfoo-1.2.4
      //
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
      assert (fpv4->changes == "some changes 1\nsome changes 2");

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
                brep::optional<dependency_constraint> (
                  dependency_constraint{comparison::ge, version ("2.0.0")})));

      // Verify 'math' repository.
      //
      assert (mr->location.canonical_name () == "cppget.org/math");
      assert (mr->location.string () ==
              "http://pkg.cppget.org/internal/1/math");
      assert (mr->display_name == "math");

      dir_path mrp (cp.directory () / dir_path ("internal/1/math"));
      assert (mr->local_path == mrp.normalize ());

      assert (mr->packages_timestamp ==
              file_mtime (dir_path (mr->local_path) / path ("packages")));
      assert (mr->repositories_timestamp ==
              file_mtime (dir_path (mr->local_path) / path ("repositories")));
      assert (mr->internal);

      shared_ptr<package> epv (
        db.load<package> (package_id ("libexp", version ("1+1.2"))));
      assert (check_location (epv));

      shared_ptr<package> fpv5 (
        db.load<package> (package_id ("libfoo", version ("1.2.4-1"))));
      assert (check_location (fpv5));

      shared_ptr<package> xpv (
        db.load<package> (package_id ("libstudxml", version ("1.0.0-1"))));
      assert (check_location (xpv));

      assert (mr->complements.empty ());
      assert (mr->prerequisites.size () == 1);
      assert (mr->prerequisites[0].load () == cr);

      // Verify libstudxml package version.
      //
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
                  dependency_constraint{comparison::ge, version ("2.0.0")})));

      assert (xpv->dependencies[1].size () == 1);
      assert (xpv->dependencies[1][0] == dep ("libgenx", nullopt));

      assert (xpv->requirements.empty ());

      // Verify libfoo package versions.
      //
      // libfoo-1.2.4-1
      //
      assert (fpv5->summary == "The Foo Math Library");
      assert (fpv5->tags == strings ({"c++", "foo", "math"}));
      assert (*fpv5->description ==
              "A modern C++ library with easy to use linear algebra and lot of "
              "optimization\ntools.\n\nThere are over 100 functions in total "
              "with an extensive test suite. The API is\nsimilar to MATLAB."
              "\n\nUseful for conversion of research code into production "
              "environments.");
      assert (fpv5->url == "http://www.example.com/foo/");
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

      const char ch[] = R"DLM(1.2.4-1
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
                brep::optional<dependency_constraint> (
                  dependency_constraint{comparison::lt, version ("1.1")})));

      assert (fpv5->dependencies[0][1] ==
              dep (
                "libmisc",
                brep::optional<dependency_constraint> (
                  dependency_constraint{comparison::gt, version ("2.3.0")})));

      assert (fpv5->dependencies[1].size () == 1);
      assert (fpv5->dependencies[1].comment.empty ());

      assert (fpv5->dependencies[1][0] ==
              dep ("libexp",
                   brep::optional<dependency_constraint> (
                     dependency_constraint{comparison::ge, version ("1.0")})));

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

      // Verify libexp package version.
      //
      // libexp-1+1.2
      //
      assert (epv->summary == "The exponent");
      assert (epv->tags == strings ({"c++", "exponent"}));
      assert (epv->description && *epv->description ==
              "The exponent math function.");
      assert (epv->url == "http://www.exp.com");
      assert (!epv->package_url);
      assert (epv->email == email ("users@exp.com"));
      assert (!epv->package_email);

      assert (epv->internal_repository.load () == mr);
      assert (epv->other_repositories.empty ());
      assert (epv->priority == priority (priority::low));
      assert (epv->changes.empty ());

      assert (epv->license_alternatives.size () == 1);
      assert (epv->license_alternatives[0].size () == 1);
      assert (epv->license_alternatives[0][0] == "MIT");

      assert (epv->dependencies.size () == 1);
      assert (epv->dependencies[0].size () == 1);
      assert (epv->dependencies[0][0] == dep ("libmisc", nullopt));

      assert (epv->requirements.empty ());

      // Verify 'misc' repository.
      //
      assert (cr->location.canonical_name () == "cppget.org/misc");
      assert (cr->location.string () ==
              "http://pkg.cppget.org/external/1/misc");
      assert (cr->display_name.empty ());

      dir_path crp (cp.directory () / dir_path ("external/1/misc"));
      assert (cr->local_path == crp.normalize ());

      assert (cr->packages_timestamp ==
              file_mtime (dir_path (cr->local_path) / path ("packages")));
      assert (cr->repositories_timestamp ==
              file_mtime (dir_path (cr->local_path) / path ("repositories")));
      assert (!cr->internal);

      shared_ptr<package> bpv (
        db.load<package> (package_id ("libbar", version ("2.3.5"))));
      assert (check_location (bpv));

      shared_ptr<package> fpv0 (
        db.load<package> (package_id ("libfoo", version ("0.1"))));
      assert (check_location (fpv0));

      shared_ptr<package> fpv6 (
        db.load<package> (package_id ("libfoo", version ("1.2.4-2"))));
      assert (check_location (fpv6));

      assert (cr->prerequisites.empty ());
      assert (cr->complements.size () == 1);
      assert (cr->complements[0].load () == tr);

      // Verify libbar package version.
      //
      // libbar-2.3.5
      //
      assert (check_external (*bpv));
      assert (bpv->other_repositories.size () == 1);
      assert (bpv->other_repositories[0].load () == cr);

      // Verify libfoo package versions.
      //
      // libfoo-0.1
      //
      assert (check_external (*fpv0));
      assert (fpv0->other_repositories.size () == 1);
      assert (fpv0->other_repositories[0].load () == cr);

      // libfoo-1.2.4-2
      //
      assert (check_external (*fpv6));
      assert (fpv6->other_repositories.size () == 1);
      assert (fpv6->other_repositories[0].load () == cr);

      // Verify 'testing' repository.
      //
      assert (tr->location.canonical_name () == "cppget.org/testing");
      assert (tr->location.string () ==
              "http://pkg.cppget.org/external/1/testing");
      assert (tr->display_name.empty ());

      dir_path trp (cp.directory () / dir_path ("external/1/testing"));
      assert (tr->local_path == trp.normalize ());

      assert (tr->packages_timestamp ==
              file_mtime (dir_path (tr->local_path) / path ("packages")));
      assert (tr->repositories_timestamp ==
              file_mtime (dir_path (tr->local_path) / path ("repositories")));
      assert (!tr->internal);

      shared_ptr<package> mpv0 (
        db.load<package> (package_id ("libmisc", version ("2.4.0"))));
      assert (check_location (mpv0));

      assert (tr->prerequisites.empty ());
      assert (tr->complements.size () == 1);
      assert (tr->complements[0].load () == gr);

      // Verify libmisc package version.
      //
      // libmisc-2.4.0
      //
      assert (check_external (*mpv0));
      assert (mpv0->other_repositories.size () == 1);
      assert (mpv0->other_repositories[0].load () == tr);

      // Verify 'staging' repository.
      //
      assert (gr->location.canonical_name () == "cppget.org/staging");
      assert (gr->location.string () ==
              "http://pkg.cppget.org/external/1/staging");
      assert (gr->display_name.empty ());

      dir_path grp (cp.directory () / dir_path ("external/1/staging"));
      assert (gr->local_path == grp.normalize ());

      assert (gr->packages_timestamp ==
              file_mtime (dir_path (gr->local_path) / path ("packages")));
      assert (gr->repositories_timestamp ==
              file_mtime (dir_path (gr->local_path) / path ("repositories")));
      assert (!gr->internal);

      shared_ptr<package> tpv (
        db.load<package> (package_id ("libexpat", version ("5.1"))));
      assert (check_location (tpv));

      shared_ptr<package> gpv (
        db.load<package> (package_id ("libgenx", version ("1.0"))));
      assert (check_location (gpv));

      shared_ptr<package> mpv1 (
        db.load<package> (package_id ("libmisc", version ("1.0"))));
      assert (check_location (mpv1));

      assert (gr->prerequisites.empty ());
      assert (gr->complements.empty ());

      // Verify libexpat package version.
      //
      // libexpat-5.1
      //
      assert (check_external (*tpv));
      assert (tpv->other_repositories.size () == 1);
      assert (tpv->other_repositories[0].load () == gr);

      // Verify libgenx package version.
      //
      // libgenx-1.0
      //
      assert (check_external (*gpv));
      assert (gpv->other_repositories.size () == 1);
      assert (gpv->other_repositories[0].load () == gr);

      // Verify libmisc package version.
      //
      // libmisc-1.0
      //
      assert (check_external (*mpv1));
      assert (mpv1->other_repositories.size () == 1);
      assert (mpv1->other_repositories[0].load () == gr);

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
      db.load<package> (package_id ("libbar", version ("2.3.5"))));

    assert (bpv->summary == "test");

    t.commit ();
  }
  // Fully qualified to avoid ambiguity with odb exception.
  //
  catch (const std::exception& e)
  {
    cerr << e.what () << endl;
    return 1;
  }
}
