// file      : tests/load/driver.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <iostream>
#include <exception>
#include <algorithm> // sort(), find()

#include <odb/session.hxx>
#include <odb/transaction.hxx>

#include <odb/pgsql/database.hxx>

#include <libbutl/process.hxx>
#include <libbutl/filesystem.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <libbrep/package.hxx>
#include <libbrep/package-odb.hxx>

#undef NDEBUG
#include <cassert>

using std::cerr;
using std::endl;

using namespace odb::core;
using namespace butl;
using namespace brep;

using labels = small_vector<string, 5>;
using req_alts = small_vector<string, 1>;

static const path packages     ("packages.manifest");
static const path repositories ("repositories.manifest");

static bool
check_location (shared_ptr<package>& p)
{
  if (p->internal ())
    return p->location && *p->location ==
      path (p->name.string () + "-" + p->version.string () + ".tar.gz");
  else
    return !p->location;
}

static bool
check_external (const package& p)
{
  return p.summary.empty ()               &&
         p.topics.empty ()                &&
         p.keywords.empty ()              &&
         !p.description                   &&
         !p.url                           &&
         !p.package_url                   &&
         !p.email                         &&
         !p.package_email                 &&
         !p.internal ()                   &&
         p.other_repositories.size () > 0 &&
         p.priority == priority ()        &&
         p.changes.empty ()               &&
         p.license_alternatives.empty ()  &&
         p.dependencies.empty ()          &&
         p.requirements.empty ()          &&
         !p.sha256sum;
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

static void
test_pkg_repos (const cstrings& loader_args,
                const dir_path& loadtab_dir,
                odb::pgsql::database&,
                const string& tenant);

static void
test_git_repos (const cstrings& loader_args,
                const dir_path& loadtab_dir,
                odb::pgsql::database&,
                const string& tenant);

int
main (int argc, char* argv[])
{
  auto print_usage = [argv]()
  {
    cerr << "usage: " << argv[0] << " (pkg|git) "
         << "<loader-path> [loader-options] <loadtab-dir>" << endl;
  };

  if (argc < 4)
  {
    print_usage ();
    return 1;
  }

  int i (1);
  repository_type rt;

  try
  {
    rt = to_repository_type (argv[i++]);
  }
  catch (const invalid_argument&)
  {
    print_usage ();
    return 1;
  }

  // Parse the tenant and database options.
  //
  string tenant;
  string user;
  string password;
  string name ("brep_package");
  string host;
  unsigned int port (0);

  for (++i; i < argc - 1; ++i)
  {
    string n (argv[i]);
    if (n == "--tenant")
      tenant = argv[++i];
    else if (n == "--db-user" || n == "-u")
      user = argv[++i];
    else if (n == "--db-password")
      password = argv[++i];
    else if (n == "--db-name" || n == "-n")
      name = argv[++i];
    else if (n == "--db-host" || n == "-h")
      host = argv[++i];
    else if (n == "--db-port" || n == "-p")
      port = std::stoul (argv[++i]);
  }

  if (i != argc - 1)
  {
    print_usage ();
    return 1;
  }

  dir_path loadtab_dir (argv[i]);

  // Make configuration file directory absolute to use it as base for internal
  // repositories relative local paths.
  //
  if (loadtab_dir.relative ())
    loadtab_dir.complete ();

  // Extract the loader args that are common for all tests (database options,
  // etc). Note that they don't contain the loadtab path and the trailing
  // NULL.
  //
  cstrings loader_args;
  for (int i (2); i != argc - 1; ++i)
    loader_args.push_back (argv[i]);

  try
  {
    odb::pgsql::database db (
      user,
      password,
      name,
      host,
      port,
      "options='-c default_transaction_isolation=serializable'");

    switch (rt)
    {
    case repository_type::pkg:
      {
        test_pkg_repos (loader_args, loadtab_dir, db, tenant);
        break;
      }
    case repository_type::git:
      {
        test_git_repos (loader_args, loadtab_dir, db, tenant);
        break;
      }
    default:
      {
        print_usage ();
        return 1;
      }
    }
  }
  // Fully qualified to avoid ambiguity with odb exception.
  //
  catch (const std::exception& e)
  {
    cerr << e << endl;
    return 1;
  }

  return 0;
}

static inline dependency
dep (const char* n, optional<version_constraint> c)
{
  return dependency {package_name (n), move (c), nullptr /* package */};
}

static inline version
dep_ver (const char* v)
{
  return version (v, version::none);
}

static void
test_git_repos (const cstrings& loader_args,
                const dir_path& loadtab_dir,
                odb::pgsql::database& db,
                const string& tenant)
{
  path loadtab (loadtab_dir / "git-loadtab");

  cstrings args (loader_args);
  args.push_back ("--force");
  args.push_back ("--shallow");
  args.push_back (loadtab.string ().c_str ());
  args.push_back (nullptr);

  {
    // Run the loader.
    //
    assert (process (args.data ()).wait ());

    // Check persistent objects.
    //
    session s;
    transaction t (db.begin ());

    assert (db.query<repository> (
              query<repository>::id.tenant == tenant).size () == 1);

    assert (db.query<package> (
              query<package>::id.tenant == tenant).size () == 1);

    // Verify 'foo' repository.
    //
    shared_ptr<repository> r (
      db.load<repository> (repository_id (tenant,
                                          "git:example.com/foo#master")));

    assert (r->location.string () == "https://git.example.com/foo.git#master");
    assert (r->summary && *r->summary == "foo project repository");
    assert (r->buildable);

    // Verify libfoo package version.
    //
    // libfoo-1.0
    //
    shared_ptr<package> p (
      db.load<package> (
        package_id (tenant, package_name ("libfoo"), version ("1.0"))));

    assert (p->fragment &&
            *p->fragment == "0f50af28d1cfb0c22f5b88e2bf674ab732e058d9");

    assert (p->dependencies.size () == 1);
    assert (p->dependencies[0].size () == 1);

    assert (p->dependencies[0][0] ==
            dep ("libmisc",
                 version_constraint (
                   dep_ver ("1.0"), false, dep_ver ("1.0"), false)));

    assert (p->buildable);

    t.commit ();
  }
}

static void
test_pkg_repos (const cstrings& loader_args,
                const dir_path& loadtab_dir,
                odb::pgsql::database& db,
                const string& tenant)
{
  path p (loadtab_dir / dir_path ("1/stable") / packages);
  timestamp srt (file_mtime (p));

  path loadtab (loadtab_dir / "loadtab");

  // Load the repositories and check persistent objects validity.
  //
  {
    cstrings args (loader_args);
    args.push_back ("--force");
    args.push_back (loadtab.string ().c_str ());
    args.push_back (nullptr);

    // Run the loader.
    //
    assert (process (args.data ()).wait ());

    // Check persistent objects.
    //
    session s;
    transaction t (db.begin ());

    assert (db.query<repository> (
              query<repository>::id.tenant == tenant).size () == 7);

    assert (db.query<package> (
              query<package>::id.tenant == tenant).size () == 21);

    shared_ptr<repository> sr (
      db.load<repository> (repository_id (tenant,
                                          "pkg:dev.cppget.org/stable")));

    shared_ptr<repository> mr (
      db.load<repository> (repository_id (tenant,
                                          "pkg:dev.cppget.org/math")));

    shared_ptr<repository> cr (
      db.load<repository> (repository_id (tenant, "pkg:dev.cppget.org/misc")));

    shared_ptr<repository> tr (
      db.load<repository> (repository_id (tenant,
                                          "pkg:dev.cppget.org/testing")));

    shared_ptr<repository> gr (
      db.load<repository> (repository_id (tenant,
                                          "pkg:dev.cppget.org/staging")));

    // Verify 'stable' repository.
    //
    assert (sr->location.canonical_name () == "pkg:dev.cppget.org/stable");
    assert (sr->location.string () ==
            "http://dev.cppget.org/1/stable");
    assert (sr->display_name == "stable");
    assert (sr->priority == 1);
    assert (!sr->interface_url);
    assert (sr->email && *sr->email == "repoman@dev.cppget.org" &&
            sr->email->comment == "public mailing list");
    assert (sr->summary &&
            *sr->summary == "General C++ package stable repository");
    assert (sr->description && *sr->description ==
            "This is the awesome C++ package repository full of exciting "
            "stuff.");

    dir_path srp (loadtab.directory () / dir_path ("1/stable"));
    assert (sr->cache_location.path () == srp.normalize ());

    assert (!sr->buildable);

    assert (sr->packages_timestamp == srt);
    assert (sr->repositories_timestamp ==
            file_mtime (sr->cache_location.path () / repositories));

    assert (sr->internal);
    assert (sr->complements.empty ());
    assert (sr->prerequisites.size () == 2);
    assert (sr->prerequisites[0].load () == cr);
    assert (sr->prerequisites[1].load () == mr);

    // Verify libfoo package versions.
    //
    // libfoo-+0-X.Y
    //
    shared_ptr<package> fpvxy (
      db.load<package> (
        package_id (tenant, package_name ("libfoo"), version ("+0-X.Y"))));

    assert (fpvxy->project == package_name ("libfoo"));
    assert (fpvxy->summary == "The Foo Library");
    assert (fpvxy->keywords.empty ());
    assert (!fpvxy->description);
    assert (!fpvxy->url);
    assert (!fpvxy->package_url);
    assert (!fpvxy->email);
    assert (!fpvxy->package_email);

    assert (fpvxy->internal_repository.load () == mr);
    assert (fpvxy->other_repositories.empty ());

    assert (fpvxy->priority == priority::low);
    assert (fpvxy->changes.empty ());

    assert (fpvxy->license_alternatives.size () == 1);
    assert (fpvxy->license_alternatives[0].size () == 1);
    assert (fpvxy->license_alternatives[0][0] == "MIT");

    assert (fpvxy->dependencies.empty ());
    assert (fpvxy->requirements.empty ());

    assert (check_location (fpvxy));

    assert (fpvxy->sha256sum && *fpvxy->sha256sum ==
            "c994fd49f051ab7fb25f3a4e68ca878e484c5d3c2cb132b37d41224b0621b618");

    assert (fpvxy->buildable);

    // libfoo-1.0
    //
    shared_ptr<package> fpv1 (
      db.load<package> (
        package_id (tenant, package_name ("libfoo"), version ("1.0"))));

    assert (fpv1->summary == "The Foo Library");
    assert (fpv1->keywords.empty ());
    assert (!fpv1->description);
    assert (!fpv1->url);
    assert (!fpv1->package_url);
    assert (!fpv1->email);
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
            "e89c6d746f8b1ea3ec58d294946d2f683d133438d2ac8c88549ba24c19627e76");

    assert (fpv1->buildable);

    // libfoo-1.2.2
    //
    shared_ptr<package> fpv2 (
      db.load<package> (
        package_id (tenant, package_name ("libfoo"), version ("1.2.2"))));

    assert (fpv2->summary == "The Foo library");

    assert (fpv2->keywords == labels ({"c++", "foo"}));
    assert (!fpv2->description);
    assert (fpv2->url && fpv2->url->string () == "http://www.example.com/foo/");
    assert (!fpv2->package_url);
    assert (fpv2->email && *fpv2->email == "foo-users@example.com");
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

    assert (fpv2->dependencies[0][0] ==
            dep ("libbar",
                 version_constraint (
                   nullopt, true, dep_ver ("2.4.0"), false)));

    assert (fpv2->dependencies[1][0] ==
            dep ("libexp",
                 version_constraint (
                   dep_ver ("+2-1.2"), false, dep_ver ("+2-1.2"), false)));

    assert (check_location (fpv2));

    assert (fpv2->sha256sum && *fpv2->sha256sum ==
            "088068ea3d69542a153f829cf836013374763148fba0a43d8047974f58b5efd7");

    assert (!fpv2->buildable);

    // libfoo-1.2.2-alpha.1
    //
    shared_ptr<package> fpv2a (
      db.load<package> (
        package_id (tenant,
                    package_name ("libfoo"),
                    version ("1.2.2-alpha.1"))));

    assert (fpv2a->summary == "The Foo library");
    assert (fpv2a->keywords == labels ({"c++", "foo"}));
    assert (!fpv2a->description);
    assert (fpv2a->url && fpv2a->url->string () == "ftp://www.example.com/foo/");
    assert (!fpv2a->package_url);
    assert (fpv2a->email && *fpv2a->email == "foo-users@example.com");
    assert (!fpv2a->package_email);

    assert (fpv2a->internal_repository.load () == sr);
    assert (fpv2a->other_repositories.empty ());
    assert (fpv2a->priority == priority::security);
    assert (fpv2a->changes.empty ());

    assert (fpv2a->license_alternatives.size () == 1);
    assert (fpv2a->license_alternatives[0].size () == 1);
    assert (fpv2a->license_alternatives[0][0] == "MIT");

    assert (fpv2a->dependencies.size () == 3);
    assert (fpv2a->dependencies[0].size () == 2);
    assert (fpv2a->dependencies[1].size () == 1);
    assert (fpv2a->dependencies[2].size () == 2);

    assert (fpv2a->dependencies[0][0] ==
            dep ("libmisc",
                 version_constraint (
                   dep_ver ("0.1"), false, dep_ver ("2.0.0-"), true)));

    assert (fpv2a->dependencies[0][1] ==
            dep ("libmisc",
                 version_constraint (
                   dep_ver ("2.0"), false, dep_ver ("5.0"), false)));

    assert (fpv2a->dependencies[1][0] ==
            dep ("libgenx",
                 version_constraint (
                   dep_ver ("0.2"), true, dep_ver ("3.0"), true)));

    assert (fpv2a->dependencies[2][0] ==
            dep ("libexpat",
                 version_constraint (
                   nullopt, true, dep_ver ("5.2"), true)));

    assert (fpv2a->dependencies[2][1] ==
            dep ("libexpat",
                 version_constraint (
                   dep_ver ("1"), true, dep_ver ("5.1"), false)));

    assert (fpv2a->requirements.empty ());

    assert (check_location (fpv2a));

    assert (fpv2a->sha256sum && *fpv2a->sha256sum ==
            "f5d3e9e6e8f9621a638b1375d31f0eb50e6279d8066170b25da21e84198cfd82");

    assert (!fpv2a->buildable);

    // libfoo-1.2.3-4
    //
    shared_ptr<package> fpv3 (
      db.load<package> (
        package_id (tenant, package_name ("libfoo"), version ("1.2.3+4"))));

    assert (fpv3->summary == "The Foo library");
    assert (fpv3->keywords == labels ({"c++", "foo"}));
    assert (!fpv3->description);
    assert (fpv3->url && fpv3->url->string () == "http://www.example.com/foo/");
    assert (!fpv3->package_url);
    assert (fpv3->email && *fpv3->email == "foo-users@example.com");
    assert (!fpv3->package_email);

    assert (fpv3->internal_repository.load () == sr);
    assert (fpv3->other_repositories.empty ());
    assert (fpv3->priority == priority::medium);

    assert (fpv3->changes.empty ());

    assert (fpv3->license_alternatives.size () == 1);
    assert (fpv3->license_alternatives[0].size () == 1);
    assert (fpv3->license_alternatives[0][0] == "MIT");

    assert (fpv3->dependencies.size () == 1);
    assert (fpv3->dependencies[0].size () == 1);
    assert (fpv3->dependencies[0][0] ==
            dep ("libmisc",
                 version_constraint (
                   dep_ver ("2.0.0"), false, nullopt, true)));

    assert (check_location (fpv3));

    assert (fpv3->sha256sum && *fpv3->sha256sum ==
            "f2ebecac6cac8addd7c623bc1becf055e76b13a0d2dd385832b92c38c58956d8");

    assert (!fpv3->buildable);

    // libfoo-1.2.4
    //
    shared_ptr<package> fpv4 (
      db.load<package> (
        package_id (tenant, package_name ("libfoo"), version ("1.2.4"))));

    assert (fpv4->summary == "The Foo Library");
    assert (fpv4->keywords == labels ({"c++", "foo"}));
    assert (*fpv4->description == "Very good foo library.");
    assert (fpv4->url && fpv4->url->string () == "http://www.example.com/foo/");
    assert (!fpv4->package_url);
    assert (fpv4->email && *fpv4->email == "foo-users@example.com");
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
            dep ("libmisc",
                 version_constraint (
                   dep_ver ("2.0.0"), false, nullopt, true)));

    assert (check_location (fpv4));

    assert (fpv4->sha256sum && *fpv4->sha256sum ==
            "aa1606323bfc59b70de642629dc5d8318cc5348e3646f90ed89406d975db1e1d");

    assert (!fpv4->buildable);

    // Verify 'math' repository.
    //
    assert (mr->location.canonical_name () == "pkg:dev.cppget.org/math");
    assert (mr->location.string () ==
            "http://dev.cppget.org/1/math");
    assert (mr->display_name == "math");
    assert (mr->priority == 2);
    assert (!mr->interface_url);
    assert (mr->email && *mr->email == "repoman@dev.cppget.org");
    assert (mr->summary && *mr->summary == "Math C++ package repository");
    assert (mr->description && *mr->description ==
            "This is the awesome C++ package repository full of remarkable "
            "algorithms and\nAPIs.");

    dir_path mrp (loadtab.directory () / dir_path ("1/math"));
    assert (mr->cache_location.path () == mrp.normalize ());

    assert (mr->buildable);

    assert (mr->packages_timestamp ==
            file_mtime (mr->cache_location.path () / packages));

    assert (mr->repositories_timestamp ==
            file_mtime (mr->cache_location.path () / repositories));

    assert (mr->internal);

    assert (mr->complements.empty ());
    assert (mr->prerequisites.size () == 1);
    assert (mr->prerequisites[0].load () == cr);

    // Verify libstudxml package version.
    //
    shared_ptr<package> xpv (
      db.load<package> (
        package_id (tenant,
                    package_name ("libstudxml"),
                    version ("1.0.0+1"))));

    assert (xpv->summary == "Modern C++ XML API");
    assert (xpv->keywords ==
            labels ({"c++", "xml", "parser", "serializer", "pull"}));
    assert (!xpv->description);
    assert (xpv->url &&
            xpv->url->string () == "http://www.codesynthesis.com/projects/libstudxml/");
    assert (!xpv->package_url);
    assert (xpv->email && *xpv->email == email ("studxml-users@example.com",
                   "Public mailing list, posts by  non-members "
                   "are allowed but moderated."));

    assert (xpv->package_email &&
            *xpv->package_email == email ("studxml-package@example.com",
                                          "Direct email to the packager."));
    assert (xpv->build_warning_email &&
            *xpv->build_warning_email ==
            email ("studxml-warnings@example.com"));

    assert (xpv->build_error_email &&
            *xpv->build_error_email ==
            email ("studxml-errors@example.com"));

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
            dep ("libexpat",
                 version_constraint (
                   dep_ver ("2.0.0"), false, nullopt, true)));

    assert (xpv->dependencies[1].size () == 1);
    assert (xpv->dependencies[1][0] == dep ("libgenx", nullopt));

    assert (xpv->requirements.empty ());

    assert (check_location (xpv));

    assert (xpv->sha256sum && *xpv->sha256sum ==
            "1833906dd93ccc0cda832d6a1b3ef9ed7877bb9958b46d9b2666033d4a7919c9");

    assert (xpv->buildable);

    // Verify libfoo package versions.
    //
    // libfoo-1.2.4+1
    //
    shared_ptr<package> fpv5 (
      db.load<package> (
        package_id (tenant, package_name ("libfoo"), version ("1.2.4+1"))));

    assert (fpv5->summary == "The Foo Math Library");
    assert (fpv5->topics ==
            labels ({"math library", "math API", "libbaz fork"}));
    assert (fpv5->keywords == labels ({"c++", "foo", "math", "best"}));

    assert (*fpv5->description ==
            "A modern C++ library with easy to use linear algebra and lot "
            "of optimization\ntools.\n\nThere are over 100 functions in "
            "total with an extensive test suite. The API is\nsimilar to "
            "~~mathlab~~ **MATLAB**.[^mathlab]\n\nUseful for conversion of "
            "research code into production environments.\n"
            "[^mathlab]: MATLAB Capabilities: TODO");

    assert (fpv5->url && fpv5->url->string () == "http://www.example.com/foo/");

    assert (fpv5->doc_url && fpv5->doc_url->string () ==
            "http://www.example.org/projects/libfoo/man.xhtml" &&
            fpv5->doc_url->comment == "Documentation page.");

    assert (fpv5->src_url && fpv5->src_url->string () ==
            "http://scm.example.com/?p=odb/libodb.git;a=tree" &&
            fpv5->src_url->comment == "Source tree url.");

    assert (fpv5->package_url &&
            fpv5->package_url->string () == "http://www.example.com/foo/pack");
    assert (fpv5->email && *fpv5->email == "foo-users@example.com");
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
            dep ("libmisc",
                 version_constraint (
                   nullopt, true, dep_ver ("1.1"), true)));

    assert (fpv5->dependencies[0][1] ==
            dep ("libmisc",
                 version_constraint (
                   dep_ver ("2.3.0+0"), true, nullopt, true)));

    assert (fpv5->dependencies[1].size () == 1);
    assert (fpv5->dependencies[1].comment.empty ());

    assert (fpv5->dependencies[1][0] ==
            dep ("libexp",
                 version_constraint (
                   dep_ver ("1.0"), false, nullopt, true)));

    assert (fpv5->dependencies[2].size () == 2);
    assert (fpv5->dependencies[2].comment == "The newer the better.");

    assert (fpv5->dependencies[2][0] == dep ("libstudxml", nullopt));
    assert (fpv5->dependencies[2][1] == dep ("libexpat", nullopt));

    requirements& fpvr5 (fpv5->requirements);
    assert (fpvr5.size () == 5);

    assert (fpvr5[0] == req_alts ({"linux", "windows", "macosx"}));
    assert (!fpvr5[0].conditional);
    assert (fpvr5[0].comment == "Symbian support is coming.");

    assert (fpvr5[1] == req_alts ({"c++11"}));
    assert (!fpvr5[1].conditional);
    assert (fpvr5[1].comment.empty ());

    assert (fpvr5[2].empty ());
    assert (fpvr5[2].conditional);
    assert (fpvr5[2].comment ==
            "libc++ standard library if using Clang on Mac OS X.");

    assert (fpvr5[3] == req_alts ({"vc++ >= 12.0"}));
    assert (fpvr5[3].conditional);
    assert (fpvr5[3].comment == "Only if using VC++ on Windows.");

    assert (fpvr5[4][0] == "host");

    assert (check_location (fpv5));

    assert (fpv5->sha256sum && *fpv5->sha256sum ==
            "f99cb46b97d0e1dccbdd10571f1f649ac5bbb22d6c25adadbc579ffbbb89d31c");

    assert (fpv5->buildable);

    // Verify libexp package version.
    //
    // libexp-+2-1.2
    //
    shared_ptr<package> epv (
      db.load<package> (
        package_id (tenant, package_name ("libexp"), version ("+2-1.2+1"))));

    assert (epv->upstream_version && *epv->upstream_version == "1.2.abc.15-x");
    assert (epv->project == "mathLab");
    assert (epv->summary == "The exponent");
    assert (epv->keywords == labels ({"mathlab", "c++", "exponent"}));
    assert (epv->description && *epv->description ==
            "The exponent math function.");
    assert (epv->url && epv->url->string () == "http://exp.example.com");
    assert (!epv->package_url);
    assert (epv->email && *epv->email == email ("users@exp.example.com"));
    assert (!epv->package_email);
    assert (epv->build_email && *epv->build_email == "builds@exp.example.com");

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
                 version_constraint (
                   dep_ver ("9.0.0"), false, nullopt, true)));

    assert (epv->requirements.empty ());

    assert (epv->buildable);

    db.load (*epv, epv->build_section);

    assert (
      epv->build_constraints ==
      build_constraints ({
          build_constraint (
            false, "windows**d", optional<string> ("x86_64**"), ""),
        build_constraint (false, "windows-vc_13**", nullopt, ""),
        build_constraint (true, "**", nullopt, "Only supported on Windows.")}));

    assert (check_location (epv));
    assert (epv->sha256sum && *epv->sha256sum ==
            "317c8c6f45d9dfdfdef3a823411920cecd51729c7c4f58f9a0b0bbd681c07bd6");

    // Verify libpq package version.
    //
    // libpq-0
    //
    shared_ptr<package> qpv (
      db.load<package> (package_id (tenant,
                                    package_name ("libpq"),
                                    version ("0"))));

    assert (qpv->summary == "PostgreSQL C API client library");
    assert (!qpv->buildable);

    // Verify 'misc' repository.
    //
    assert (cr->location.canonical_name () == "pkg:dev.cppget.org/misc");
    assert (cr->location.string () ==
            "http://dev.cppget.org/1/misc");
    assert (cr->display_name.empty ());
    assert (cr->priority == 0);
    assert (cr->interface_url &&
            *cr->interface_url == "http://misc.cppget.org/");
    assert (!cr->email);
    assert (!cr->summary);
    assert (!cr->description);

    dir_path crp (loadtab.directory () / dir_path ("1/misc"));
    assert (cr->cache_location.path () == crp.normalize ());

    assert (!cr->buildable);

    assert (cr->packages_timestamp ==
            file_mtime (cr->cache_location.path () / packages));

    assert (cr->repositories_timestamp ==
            file_mtime (cr->cache_location.path () / repositories));

    assert (!cr->internal);
    assert (cr->prerequisites.empty ());
    assert (cr->complements.size () == 1);
    assert (cr->complements[0].load () == tr);

    // Verify libbar package version.
    //
    // libbar-2.4.0+3
    //
    shared_ptr<package> bpv (
      db.load<package> (
        package_id (tenant, package_name ("libbar"), version ("2.4.0+3"))));

    assert (check_external (*bpv));
    assert (bpv->other_repositories.size () == 1);
    assert (bpv->other_repositories[0].load () == cr);
    assert (check_location (bpv));

    assert (!bpv->buildable);

    // Verify libfoo package versions.
    //
    // libfoo-0.1
    //
    shared_ptr<package> fpv0 (
      db.load<package> (
        package_id (tenant, package_name ("libfoo"), version ("0.1"))));

    assert (check_external (*fpv0));
    assert (fpv0->other_repositories.size () == 1);
    assert (fpv0->other_repositories[0].load () == cr);
    assert (check_location (fpv0));

    assert (!fpv0->buildable);

    // libfoo-1.2.4+2
    //
    shared_ptr<package> fpv6 (
      db.load<package> (
        package_id (tenant, package_name ("libfoo"), version ("1.2.4+2"))));

    assert (check_external (*fpv6));
    assert (fpv6->other_repositories.size () == 1);
    assert (fpv6->other_repositories[0].load () == cr);
    assert (check_location (fpv6));

    assert (!fpv6->buildable);

    // Verify 'testing' repository.
    //
    assert (tr->location.canonical_name () == "pkg:dev.cppget.org/testing");
    assert (tr->location.string () ==
            "http://dev.cppget.org/1/testing");
    assert (tr->display_name == "testing");
    assert (tr->priority == 3);
    assert (tr->interface_url &&
            *tr->interface_url == "http://test.cppget.org/hello/");
    assert (!tr->email);
    assert (!tr->summary);
    assert (!tr->description);

    dir_path trp (loadtab.directory () / dir_path ("1/testing"));
    assert (tr->cache_location.path () == trp.normalize ());

    assert (!tr->buildable);

    assert (tr->packages_timestamp ==
            file_mtime (tr->cache_location.path () / packages));

    assert (tr->repositories_timestamp ==
            file_mtime (tr->cache_location.path () / repositories));

    assert (tr->internal);
    assert (tr->prerequisites.empty ());
    assert (tr->complements.size () == 1);
    assert (tr->complements[0].load () == gr);

    // Verify libmisc package version.
    //
    // libmisc-2.4.0
    //
    shared_ptr<package> mpv0 (
      db.load<package> (
        package_id (tenant, package_name ("libmisc"), version ("2.4.0"))));

    assert (mpv0->internal_repository.load () == tr);
    assert (mpv0->other_repositories.empty ());
    assert (check_location (mpv0));
    assert (!mpv0->buildable);

    // libmisc-2.3.0+1
    //
    shared_ptr<package> mpv1 (
      db.load<package> (
        package_id (tenant, package_name ("libmisc"), version ("2.3.0+1"))));

    assert (mpv1->internal_repository.load () == tr);
    assert (mpv1->other_repositories.empty ());
    assert (check_location (mpv1));
    assert (!mpv1->buildable);

    // Verify 'staging' repository.
    //
    assert (gr->location.canonical_name () == "pkg:dev.cppget.org/staging");
    assert (gr->location.string () ==
            "http://dev.cppget.org/1/staging");
    assert (gr->display_name.empty ());
    assert (gr->priority == 0);
    assert (gr->interface_url &&
            *gr->interface_url == "http://dev.cppget.org/");
    assert (!gr->email);
    assert (!gr->summary);
    assert (!gr->description);

    dir_path grp (loadtab.directory () / dir_path ("1/staging"));
    assert (gr->cache_location.path () == grp.normalize ());
    assert (!gr->buildable);

    assert (gr->packages_timestamp ==
            file_mtime (gr->cache_location.path () / packages));

    assert (gr->repositories_timestamp ==
            file_mtime (gr->cache_location.path () / repositories));

    assert (!gr->internal);
    assert (gr->prerequisites.empty ());
    assert (gr->complements.empty ());

    // Verify libexpat package version.
    //
    // libexpat-5.1
    //
    shared_ptr<package> tpv (
      db.load<package> (
        package_id (tenant, package_name ("libexpat"), version ("5.1"))));

    assert (check_external (*tpv));
    assert (tpv->other_repositories.size () == 1);
    assert (tpv->other_repositories[0].load () == gr);
    assert (check_location (tpv));
    assert (!tpv->buildable);

    // Verify libgenx package version.
    //
    // libgenx-1.0
    //
    shared_ptr<package> gpv (
      db.load<package> (
        package_id (tenant, package_name ("libgenx"), version ("1.0"))));

    assert (check_external (*gpv));
    assert (gpv->other_repositories.size () == 1);
    assert (gpv->other_repositories[0].load () == gr);
    assert (check_location (gpv));
    assert (!gpv->buildable);

    // Verify libmisc package version.
    //
    // libmisc-1.0
    //
    shared_ptr<package> mpv2 (
      db.load<package> (
        package_id (tenant, package_name ("libmisc"), version ("1.0"))));

    assert (check_external (*mpv2));
    assert (mpv2->other_repositories.size () == 1);
    assert (mpv2->other_repositories[0].load () == gr);
    assert (check_location (mpv2));
    assert (!mpv2->buildable);

    // Change package summary, update the object persistent state, rerun
    // the loader and make sure the model were not rebuilt.
    //
    bpv->summary = "test";
    db.update (bpv);

    t.commit ();
  }

  // Rerun the loader without --force and make sure the model were not
  // rebuilt.
  //
  {
    cstrings args (loader_args);
    args.push_back (loadtab.string ().c_str ());
    args.push_back (nullptr);

    assert (process (args.data ()).wait ());

    transaction t (db.begin ());

    shared_ptr<package> bpv (
      db.load<package> (
        package_id (tenant, package_name ("libbar"), version ("2.4.0+3"))));

    assert (bpv->summary == "test");

    t.commit ();
  }

  // Restore the original setup.
  //
  {
    cstrings args (loader_args);
    args.push_back ("--force");
    args.push_back (loadtab.string ().c_str ());
    args.push_back (nullptr);

    assert (process (args.data ()).wait ());

    transaction t (db.begin ());

    shared_ptr<package> bpv (
      db.find<package> (
        package_id (tenant, package_name ("libbar"), version ("2.4.0+3"))));

    // External package summary is not saved.
    //
    assert (bpv->summary.empty ());

    t.commit ();
  }
}
