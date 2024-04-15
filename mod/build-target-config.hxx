// file      : mod/build-target-config.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_BUILD_TARGET_CONFIG_HXX
#define MOD_BUILD_TARGET_CONFIG_HXX

#include <map>

#include <libbutl/target-triplet.hxx>

#include <libbpkg/manifest.hxx>

#include <libbbot/build-target-config.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <libbrep/common.hxx>

namespace brep
{
  using build_target_config  = bbot::build_target_config;
  using build_target_configs = bbot::build_target_configs;

  // Return true if the specified build target configuration is excluded by a
  // package configuration based on its underlying build class set, build
  // class expressions, and build constraints, potentially extending the
  // underlying set with the special classes. Set the exclusion reason if
  // requested. Optionally use the `all` class as a default underlying build
  // class set rather than the `default` class (which is, for example, the
  // case for the external test packages not to reduce their build target
  // configuration set needlessly).
  //
  bool
  exclude (const build_class_exprs& builds,
           const build_constraints& constraints,
           const build_target_config&,
           const std::map<string, string>& class_inheritance_map,
           string* reason = nullptr,
           bool default_all_ucs = false);

  template <typename K>
  inline bool
  exclude (const build_package_config_template<K>& pc,
           const build_class_exprs& common_builds,
           const build_constraints& common_constraints,
           const build_target_config& tc,
           const std::map<string, string>& class_inheritance_map,
           string* reason = nullptr,
           bool default_all_ucs = false)
  {
    return exclude (pc.effective_builds (common_builds),
                    pc.effective_constraints (common_constraints),
                    tc,
                    class_inheritance_map,
                    reason,
                    default_all_ucs);
  }

  // Convert dash-separated components (target, build target configuration
  // name, machine name) or a pattern thereof into a path, replacing dashes
  // with slashes (directory separators), `**` with `*/**/*`, and appending
  // the trailing slash for a subsequent match using the path_match()
  // functionality (the idea here is for `linux**` to match `linux-gcc` which
  // is quite natural to expect). Throw invalid_path if the resulting path is
  // invalid.
  //
  // Note that the match_absent path match flag must be used for the above
  // `**` transformation to work.
  //
  path
  dash_components_to_path (const string&);

  // Build target/target configuration name combination that, in particular,
  // identifies configurations in the buildtab and thus can be used as a
  // set/map key.
  //
  // Note: contains shallow references to the target and configuration name.
  //
  struct build_target_config_id
  {
    reference_wrapper<const butl::target_triplet> target;
    reference_wrapper<const string> config;

    bool
    operator< (const build_target_config_id& x) const
    {
      if (int r = target.get ().compare (x.target.get ()))
        return r < 0;

      return config.get ().compare (x.config.get ()) < 0;
    }
  };
}

#endif // MOD_BUILD_TARGET_CONFIG
