// file      : mod/build-config.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_BUILD_CONFIG_HXX
#define MOD_BUILD_CONFIG_HXX

#include <map>

#include <libbutl/target-triplet.hxx>

#include <libbpkg/manifest.hxx>

#include <libbbot/build-config.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

namespace brep
{
  // Return true if the specified build configuration is excluded by a package
  // based on its underlying build class set, build class expressions, and
  // build constraints, potentially extending the underlying set with the
  // special classes. Set the exclusion reason if requested. Optionally use
  // the `all` class as a default underlying build class set rather than the
  // `default` class (which is, for example, the case for the external test
  // packages not to reduce their build configuration set needlessly).
  //
  bool
  exclude (const small_vector<bpkg::build_class_expr, 1>&,
           const vector<bpkg::build_constraint>&,
           const bbot::build_config&,
           const std::map<string, string>& class_inheritance_map,
           string* reason = nullptr,
           bool default_all_ucs = false);

  // Convert dash-separated components (target, build configuration name,
  // machine name) or a pattern thereof into a path, replacing dashes with
  // slashes (directory separators), `**` with `*/**/*`, and appending the
  // trailing slash for a subsequent match using the path_match()
  // functionality (the idea here is for `linux**` to match `linux-gcc` which
  // is quite natural to expect). Throw invalid_path if the resulting path is
  // invalid.
  //
  // Note that the match_absent path match flag must be used for the above
  // `**` transformation to work.
  //
  path
  dash_components_to_path (const string&);

  // Build configuration name/target combination that, in particular,
  // identifies configurations in the buildtab and thus can be used as a
  // set/map key.
  //
  // Note: contains shallow references to the configuration name and target.
  //
  struct build_config_id
  {
    reference_wrapper<const string> name;
    reference_wrapper<const butl::target_triplet> target;

    bool
    operator< (const build_config_id& x) const
    {
      if (int r = name.get ().compare (x.name.get ()))
        return r < 0;

      return target.get ().compare (x.target.get ()) < 0;
    }
  };
}

#endif // MOD_BUILD_CONFIG
