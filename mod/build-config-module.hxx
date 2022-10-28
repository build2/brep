// file      : mod/build-config-module.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_BUILD_CONFIG_MODULE_HXX
#define MOD_BUILD_CONFIG_MODULE_HXX

#include <map>

#include <libbutl/target-triplet.hxx>

#include <libbpkg/manifest.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/module-options.hxx>
#include <mod/build-target-config.hxx>

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

    bool
    exclude (const build_package_config& pc,
             const build_class_exprs& common_builds,
             const build_constraints& common_constraints,
             const build_target_config& tc,
             string* reason = nullptr,
             bool default_all_ucs = false) const
    {
      return brep::exclude (pc,
                            common_builds,
                            common_constraints,
                            tc,
                            target_conf_->class_inheritance_map,
                            reason,
                            default_all_ucs);
    }

    // Check if the configuration belongs to the specified class.
    //
    bool
    belongs (const build_target_config&, const char*) const;

    bool
    belongs (const build_target_config& cfg, const string& cls) const
    {
      return belongs (cfg, cls.c_str ());
    }

    // Target/configuration/toolchain combination that, in particular, can be
    // used as a set value.
    //
    // Note: all members are the shallow references.
    //
    struct config_toolchain
    {
      const butl::target_triplet& target;
      const string& target_config;
      const string& package_config;
      const string& toolchain_name;
      const bpkg::version& toolchain_version;

      // Note: the comparison reflects the order of unbuilt configurations on
      // the Builds page.
      //
      bool
      operator< (const config_toolchain& ct) const
      {
        if (int r = toolchain_name.compare (ct.toolchain_name))
          return r < 0;

        if (toolchain_version != ct.toolchain_version)
          return toolchain_version > ct.toolchain_version;

        if (int r = target.compare (ct.target))
          return r < 0;

        if (int r = target_config.compare (ct.target_config))
          return r < 0;

        return package_config.compare (ct.package_config) < 0;
      }
    };

  protected:
    // Build configurations.
    //
    shared_ptr<const build_target_configs> target_conf_;

    shared_ptr<const std::map<build_target_config_id,
                              const build_target_config*>>
    target_conf_map_;

    // Map of build bot agent public keys fingerprints to the key file paths.
    //
    shared_ptr<const std::map<string, path>> bot_agent_key_map_;
  };
}

#endif // MOD_BUILD_CONFIG_MODULE_HXX
