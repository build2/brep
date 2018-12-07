// file      : mod/build-config-module.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_BUILD_CONFIG_MODULE_HXX
#define MOD_BUILD_CONFIG_MODULE_HXX

#include <map>
#include <algorithm> // find()

#include <libbutl/utility.mxx> // compare_c_string

#include <libbpkg/manifest.hxx>

#include <libbbot/build-config.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/module.hxx>
#include <mod/options.hxx>

// Base class for modules that utilize the build controller configuration.
//
// Specifically, it loads build controller configuration and provides various
// build configuration-related utilities. Note that the configuration is
// shared across multiple modules once loaded.
//
// Note that the build database is in the database_module.
//
namespace brep
{
  class build_config_module
  {
  protected:
    // Parse build configuration file and establish mapping of build bot agent
    // public keys fingerprints to their paths. Throw tab_parsing on parsing
    // error, system_error on the underlying OS error.
    //
    void
    init (const options::build&);

    // Return true if the specified build configuration is excluded by a
    // package based on its underlying build class set, build class
    // expressions, and build constraints, potentially extending the
    // underlying set with the special classes. Set the exclusion reason if
    // requested.
    //
    bool
    exclude (const vector<bpkg::build_class_expr>&,
             const vector<bpkg::build_constraint>&,
             const bbot::build_config&,
             string* reason = nullptr) const;

    // Check if the configuration belongs to the specified class.
    //
    // Note that the configuration base classes are not checked.
    //
    static bool
    belongs (const bbot::build_config& cfg, const char* cls)
    {
      const strings& cs (cfg.classes);
      return std::find (cs.begin (), cs.end (), cls) != cs.end ();
    }

    // Convert dash-separated components (target, build configuration name,
    // machine name) or a pattern thereof into a path, replacing dashes with
    // slashes (directory separators), `**` with `*/**/*`, and appending the
    // trailing slash for a subsequent match using the path_match()
    // functionality (the idea here is for `linux**` to match `linux-gcc`
    // which is quite natural to expect). Throw invalid_path if the resulting
    // path is invalid.
    //
    // Note that the match_absent path match flag must be used for the above
    // `**` transformation to work.
    //
    static path
    dash_components_to_path (const string&);

  protected:
    // Build configurations.
    //
    shared_ptr<const bbot::build_configs> build_conf_;
    shared_ptr<const cstrings>            build_conf_names_;

    shared_ptr<const std::map<const char*,
                              const bbot::build_config*,
                              butl::compare_c_string>> build_conf_map_;

    // Map of build bot agent public keys fingerprints to the key file paths.
    //
    shared_ptr<const std::map<string, path>> bot_agent_key_map_;
  };
}

#endif // MOD_BUILD_CONFIG_MODULE_HXX
