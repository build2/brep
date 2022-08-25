// file      : mod/build-config-module.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_BUILD_CONFIG_MODULE_HXX
#define MOD_BUILD_CONFIG_MODULE_HXX

#include <map>

#include <libbutl/target-triplet.hxx>

#include <libbpkg/manifest.hxx>

#include <libbbot/build-config.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/build-config.hxx>
#include <mod/module-options.hxx>

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
    exclude (const small_vector<bpkg::build_class_expr, 1>& exprs,
             const vector<bpkg::build_constraint>& constrs,
             const bbot::build_config& cfg,
             string* reason = nullptr,
             bool default_all_ucs = false) const
    {
      return brep::exclude (exprs,
                            constrs,
                            cfg,
                            build_conf_->class_inheritance_map,
                            reason,
                            default_all_ucs);
    }

    // Check if the configuration belongs to the specified class.
    //
    bool
    belongs (const bbot::build_config&, const char*) const;

    bool
    belongs (const bbot::build_config& cfg, const string& cls) const
    {
      return belongs (cfg, cls.c_str ());
    }

    // Configuration/target/toolchain combination that, in particular, can be
    // used as a set value.
    //
    // Note: contains shallow references to the configuration, target,
    // toolchain name, and version.
    //
    struct config_toolchain
    {
      const string& configuration;
      const butl::target_triplet& target;
      const string& toolchain_name;
      const bpkg::version& toolchain_version;

      bool
      operator< (const config_toolchain& ct) const
      {
        if (int r = toolchain_name.compare (ct.toolchain_name))
          return r < 0;

        if (toolchain_version != ct.toolchain_version)
          return toolchain_version > ct.toolchain_version;

        if (int r = configuration.compare (ct.configuration))
          return r < 0;

        return target.compare (ct.target) < 0;
      }
    };

  protected:
    // Build configurations.
    //
    shared_ptr<const bbot::build_configs> build_conf_;

    shared_ptr<const std::map<build_config_id, const bbot::build_config*>>
    build_conf_map_;

    // Map of build bot agent public keys fingerprints to the key file paths.
    //
    shared_ptr<const std::map<string, path>> bot_agent_key_map_;
  };
}

#endif // MOD_BUILD_CONFIG_MODULE_HXX
