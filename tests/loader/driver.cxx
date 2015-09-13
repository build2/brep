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
  return a.package == b.package && !a.version == !b.version &&
    (!a.version || (a.version->value == b.version->value &&
                    a.version->operation == b.version->operation));
}

static bool
check_location (shared_ptr<package_version>& pv)
{
  if (pv->repository.load ()->internal)
    return pv->location && *pv->location ==
      path (pv->package.load ()->name + "-" + pv->version.string () +
            ".tar.gz");
  else
    return !pv->location;
}

int
main (int argc, char* argv[])
{
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

      assert (db.query<repository> ().size () == 3);
      assert (db.query<package> ().size () == 4);
      assert (db.query<package_version> ().size () == 8);

      shared_ptr<repository> sr (db.load<repository> ("cppget.org/stable"));
      shared_ptr<repository> mr (db.load<repository> ("cppget.org/math"));
      shared_ptr<repository> cr (db.load<repository> ("cppget.org/misc"));

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
      assert (sr->prerequisite_repositories.size () == 2);

      vector<shared_ptr<repository>> pr {mr, cr};

      auto i (find (pr.begin (),
                    pr.end (),
                    sr->prerequisite_repositories[0].load ()));

      assert (i != pr.end ());
      pr.erase (i);

      assert (find (pr.begin (),
                    pr.end (),
                    sr->prerequisite_repositories[1].load ()) != pr.end ());

      auto& srv (sr->package_versions);
      assert (srv.size () == 5);

      using lv_t = decltype (srv[0]);
      auto vc ([](const lv_t& a, const lv_t& b){
          auto v1 (a.load ());
          auto v2 (b.load ());

          int r (v1->package.object_id ().compare (v2->package.object_id ()));

          if (r)
            return r < 0;

          return v1->version < v2->version;
        });

      sort (srv.begin (), srv.end (), vc);

      version fv0 ("1.0");
      shared_ptr<package_version> fpv0 (
        db.load<package_version> (
          package_version_id {
            "cppget.org/stable",
            "libfoo",
            fv0.epoch (),
            fv0.canonical_upstream ()}));
      assert (srv[0].load () == fpv0);
      assert (check_location (fpv0));

      version fv1 ("1.2.2");
      shared_ptr<package_version> fpv1 (
        db.load<package_version> (
          package_version_id {
            "cppget.org/stable",
            "libfoo",
            fv1.epoch (),
            fv1.canonical_upstream ()}));
      assert (srv[1].load () == fpv1);
      assert (check_location (fpv1));

      version fv2 ("1.2.3-4");
      shared_ptr<package_version> fpv2 (
        db.load<package_version> (
          package_version_id {
            "cppget.org/stable",
            "libfoo",
            fv2.epoch (),
            fv2.canonical_upstream ()}));
      assert (srv[2].load () == fpv2);
      assert (check_location (fpv2));

      version fv3 ("1.2.4");
      shared_ptr<package_version> fpv3 (
        db.load<package_version> (
          package_version_id {
            "cppget.org/stable",
            "libfoo",
            fv3.epoch (),
            fv3.canonical_upstream ()}));
      assert (srv[3].load () == fpv3);
      assert (check_location (fpv3));

      version xv ("1.0.0-1");
      shared_ptr<package_version> xpv (
        db.load<package_version> (
          package_version_id {
            "cppget.org/stable",
            "libstudxml",
            xv.epoch (),
            xv.canonical_upstream ()}));
      assert (srv[4].load () == xpv);
      assert (check_location (xpv));

      // Verify libstudxml package.
      //
      shared_ptr<package> px (db.load<package> ("libstudxml"));
      assert (px->name == "libstudxml");
      assert (px->summary == "Modern C++ XML API");
      assert (px->tags == strings ({"c++", "xml", "parser", "serializer",
              "pull", "streaming", "modern"}));
      assert (!px->description);
      assert (px->url == "http://www.codesynthesis.com/projects/libstudxml/");
      assert (!px->package_url);
      assert (px->email == email ("studxml-users@codesynthesis.com",
                                  "Public mailing list, posts by  non-members "
                                  "are allowed but moderated."));
      assert (px->package_email &&
              *px->package_email == email ("boris@codesynthesis.com",
                                           "Direct email to the author."));

      auto& xpvs (px->versions);
      assert (xpvs.size () == 1);
      assert (xpvs[0].load () == xpv);

      // Verify libstudxml package version.
      //
      assert (xpv->repository.load () == sr);
      assert (xpv->package.load () == px);
      assert (xpv->version == version ("1.0.0-1"));
      assert (xpv->priority == priority::low);
      assert (xpv->changes.empty ());

      assert (xpv->license_alternatives.size () == 1);
      assert (xpv->license_alternatives[0].size () == 1);
      assert (xpv->license_alternatives[0][0] == "MIT");

      assert (xpv->dependencies.size () == 2);
      assert (xpv->dependencies[0].size () == 1);
      assert (xpv->dependencies[0][0] ==
              (dependency {
                 "libexpat",
                 brep::optional<version_comparison> (
                   version_comparison{version ("2.0.0"), comparison::ge})}));

      assert (xpv->dependencies[1].size () == 1);
      assert (xpv->dependencies[1][0] ==
              (dependency {"libgenx", brep::optional<version_comparison> ()}));

      assert (xpv->requirements.empty ());

      // Verify libfoo package.
      //
      shared_ptr<package> pf (db.load<package> ("libfoo"));
      assert (pf->name == "libfoo");
      assert (pf->summary == "The Foo Math Library");
      assert (pf->tags == strings ({"c++", "foo", "math"}));
      assert (*pf->description ==
              "A modern C++ library with easy to use linear algebra and "
              "optimization tools. There are over 100 functions in total with "
              "an extensive test suite. The API is similar to MATLAB.");
      assert (pf->url == "http://www.example.com/foo/");
      assert (pf->package_url &&
              *pf->package_url == "http://www.example.com/foo/pack");
      assert (pf->email == "foo-users@example.com");
      assert (pf->package_email && *pf->package_email == "pack@example.com");

      auto& fpv (pf->versions);
      assert (fpv.size () == 5);
      sort (fpv.begin (), fpv.end (), vc);
      assert (fpv[0].load () == fpv0);
      assert (fpv[1].load () == fpv1);
      assert (fpv[2].load () == fpv2);
      assert (fpv[3].load () == fpv3);
      // Asserting fpv[3].load () == fpv4 goes later when fpv4, belonging to
      // another repository, get introduced.
      //

      // Verify libfoo package versions.
      //
      assert (fpv0->repository.load () == sr);
      assert (fpv0->package.load () == pf);
      assert (fpv0->version == version ("1.0"));
      assert (fpv0->priority == priority::low);
      assert (fpv0->changes.empty ());

      assert (fpv0->license_alternatives.size () == 1);
      assert (fpv0->license_alternatives[0].size () == 1);
      assert (fpv0->license_alternatives[0][0] == "MIT");

      assert (fpv0->dependencies.empty ());
      assert (fpv0->requirements.empty ());

      assert (fpv1->repository.load () == sr);
      assert (fpv1->package.load () == pf);
      assert (fpv1->version == version ("1.2.2"));
      assert (fpv1->priority == priority::low);
      assert (fpv1->changes.empty ());

      assert (fpv1->license_alternatives.size () == 1);
      assert (fpv1->license_alternatives[0].size () == 1);
      assert (fpv1->license_alternatives[0][0] == "MIT");

      assert (fpv1->dependencies.size () == 2);
      assert (fpv1->dependencies[0].size () == 1);
      assert (fpv1->dependencies[1].size () == 1);

      assert (fpv1->dependencies[0][0] ==
              (dependency {
                 "libbar",
                 brep::optional<version_comparison> (
                   version_comparison{version ("2.4.0"), comparison::le})}));

      assert (fpv1->dependencies[1][0] ==
              (dependency {
                 "libexp",
                 brep::optional<version_comparison> (
                   version_comparison{version ("1+1.2"), comparison::eq})}));

      requirements& fpvr1 (fpv1->requirements);
      assert (fpvr1.size () == 4);

      assert (fpvr1[0] == strings ({"linux", "windows", "macosx"}));
      assert (!fpvr1[0].conditional);
      assert (fpvr1[0].comment.empty ());

      assert (fpvr1[1] == strings ({"c++11"}));
      assert (!fpvr1[1].conditional);
      assert (fpvr1[1].comment.empty ());

      assert (fpvr1[2].empty ());
      assert (fpvr1[2].conditional);
      assert (fpvr1[2].comment == "VC++ 12.0 or later if targeting Windows.");

      assert (fpvr1[3].empty ());
      assert (fpvr1[3].conditional);
      assert (fpvr1[3].comment ==
              "libc++ standard library if using Clang on Mac OS X.");

      assert (fpv2->repository.load () == sr);
      assert (fpv2->package.load () == pf);
      assert (fpv2->version == version ("1.2.3-4"));
      assert (fpv2->priority == priority::low);
      assert (fpv2->changes.empty ());

      assert (fpv2->license_alternatives.size () == 1);
      assert (fpv2->license_alternatives[0].size () == 1);
      assert (fpv2->license_alternatives[0][0] == "MIT");

      assert (fpv2->dependencies.size () == 1);
      assert (fpv2->dependencies[0].size () == 1);
      assert (fpv2->dependencies[0][0] ==
              (dependency {
                 "libmisc",
                 brep::optional<version_comparison> (
                   version_comparison{version ("2.0.0"), comparison::ge})}));

      assert (fpv3->repository.load () == sr);
      assert (fpv3->package.load () == pf);
      assert (fpv3->version == version ("1.2.4"));
      assert (fpv3->priority == priority::low);
      assert (fpv3->changes == "some changes 1\nsome changes 2");

      assert (fpv3->license_alternatives.size () == 1);
      assert (fpv3->license_alternatives[0].size () == 1);
      assert (fpv3->license_alternatives[0][0] == "MIT");

      assert (fpv3->dependencies.size () == 1);
      assert (fpv3->dependencies[0].size () == 1);
      assert (fpv3->dependencies[0][0] ==
              (dependency {
                 "libmisc",
                 brep::optional<version_comparison> (
                   version_comparison{version ("2.0.0"), comparison::ge})}));

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
      assert (mr->prerequisite_repositories.size () == 1);
      assert (mr->prerequisite_repositories[0].load () == cr);

      auto& mrv (mr->package_versions);
      assert (mrv.size () == 2);
      sort (mrv.begin (), mrv.end (), vc);

      version ev ("1+1.2");
      shared_ptr<package_version> epv (
        db.load<package_version> (
          package_version_id {
            "cppget.org/math",
            "libexp",
            ev.epoch (),
            ev.canonical_upstream ()}));
      assert (mrv[0].load () == epv);
      assert (check_location (epv));

      version fv4 ("1.2.4-1");
      shared_ptr<package_version> fpv4 (
        db.load<package_version> (
          package_version_id {
            "cppget.org/math",
            "libfoo",
            fv4.epoch (),
            fv4.canonical_upstream ()}));
      assert (mrv[1].load () == fpv4);
      assert (fpv[4].load () == fpv4);
      assert (check_location (fpv4));

      // Verify libfoo package versions.
      //
      assert (fpv4->repository.load () == mr);
      assert (fpv4->package.load () == pf);
      assert (fpv4->version == version ("1.2.4-1"));
      assert (fpv4->priority == priority::high);
      assert (fpv4->changes.empty ());

      assert (fpv4->license_alternatives.size () == 2);
      assert (fpv4->license_alternatives[0].comment ==
              "If using with GNU TLS.");
      assert (fpv4->license_alternatives[0].size () == 2);
      assert (fpv4->license_alternatives[0][0] == "LGPLv2");
      assert (fpv4->license_alternatives[0][1] == "MIT");
      assert (fpv4->license_alternatives[1].comment ==
              "If using with OpenSSL.");
      assert (fpv4->license_alternatives[1].size () == 1);
      assert (fpv4->license_alternatives[1][0] == "BSD");

      assert (fpv4->dependencies.size () == 1);
      assert (fpv4->dependencies[0].size () == 1);

      assert (fpv4->dependencies[0][0] ==
              (dependency {
                 "libmisc",
                 brep::optional<version_comparison> (
                   version_comparison{version ("2.3.0"), comparison::ge})}));

      assert (fpv4->requirements.empty ());

      // Verify libexp package.
      //
      shared_ptr<package> pe (db.load<package> ("libexp"));
      assert (pe->name == "libexp");
      assert (pe->summary == "The exponent");
      assert (pe->tags == strings ({"c++", "exponent"}));
      assert (!pe->description);
      assert (pe->url == "http://www.exp.com");
      assert (!pe->package_url);
      assert (pe->email == email ("users@exp.com"));
      assert (!pe->package_email);

      auto& epvs (pe->versions);
      assert (epvs.size () == 1);
      assert (epvs[0].load () == epv);

      // Verify libexp package version.
      //
      assert (epv->repository.load () == mr);
      assert (epv->package.load () == pe);
      assert (epv->version == version ("1+1.2"));
      assert (epv->priority == priority (priority::low));
      assert (epv->changes.empty ());

      assert (epv->license_alternatives.size () == 1);
      assert (epv->license_alternatives[0].size () == 1);
      assert (epv->license_alternatives[0][0] == "MIT");

      assert (epv->dependencies.size () == 1);
      assert (epv->dependencies[0].size () == 1);
      assert (epv->dependencies[0][0] ==
              (dependency {"libmisc", brep::optional<version_comparison> ()}));

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
      assert (cr->repositories_timestamp == timestamp_nonexistent);
      assert (!cr->internal);
      assert (cr->prerequisite_repositories.empty ());

      auto& crv (cr->package_versions);
      assert (crv.size () == 1);

      version bv ("2.3.5");
      shared_ptr<package_version> bpv (
        db.load<package_version> (
          package_version_id {
            "cppget.org/misc",
            "libbar",
            bv.epoch (),
            bv.canonical_upstream ()}));
      assert (crv[0].load () == bpv);
      assert (check_location (bpv));

      // Verify libbar package.
      //
      shared_ptr<package> pb (db.load<package> ("libbar"));
      assert (pb->name == "libbar");
      assert (pb->summary == "The Bar library");
      assert (pb->tags == strings ({"c++", "bar"}));
      assert (!pb->description);
      assert (pb->url == "http://www.example.com/bar/");
      assert (!pb->package_url);
      assert (pb->email == email ("bar-users@example.com"));
      assert (!pb->package_email);

      auto& bpvs (pb->versions);
      assert (bpvs.size () == 1);
      assert (bpvs[0].load () == bpv);

      // Verify libbar package version.
      //
      assert (bpv->repository.load () == cr);
      assert (bpv->package.load () == pb);
      assert (bpv->version == version ("2.3.5"));

      assert (bpv->priority == priority (priority::security,
                                         "Very important to install."));
      assert (bpv->changes.empty ());

      assert (bpv->license_alternatives.size () == 1);
      assert (bpv->license_alternatives[0].size () == 1);
      assert (bpv->license_alternatives[0][0] == "GPLv2");

      assert (bpv->dependencies.empty ());
      assert (bpv->requirements.empty ());

      // Update package summary, update package persistent state, rerun loader
      // and ensure the model were not rebuilt.
      //
      pb->summary = "test";
      db.update (pb);

      t.commit ();
    }

    assert (process (ld_args).wait ());

    transaction t (db.begin ());
    assert (db.load<package> ("libbar")->summary == "test");
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
