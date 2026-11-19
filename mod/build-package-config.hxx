// file      : mod/build-package-config.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_BUILD_PACKAGE_CONFIG_HXX
#define MOD_BUILD_PACKAGE_CONFIG_HXX

#include <map>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

namespace brep
{
  // Combine the base package build configuration name (as it appears in the
  // package manifest) and the potentially empty configuration variant name
  // (as it appears in the build-package-config-variant brep configuration
  // option) into the following package configuration name form (as it appears
  // in the build id):
  //
  // <base-name>[+<variant-name>]
  //
  // Note: the trailing part is only added if the variant name is not empty.
  //
  inline string
  package_build_config_name (const string& base_name,
                             const string& variant_name)
  {
    return !variant_name.empty ()
      ? base_name + '+' + variant_name
      : base_name;
  }

  // Combine the base package build configuration arguments and configuration
  // variant arguments. Separate the parts with the newline, if both are not
  // empty.
  //
  inline string
  package_build_config_arguments (const string& base_args,
                                  const string& variant_args)
  {
    string r (base_args);

    if (!variant_args.empty ())
    {
      if (!r.empty ())
        r += '\n';

      r += variant_args;
    }

    return r;
  }

  // Split the package build configuration name, created by the above
  // package_build_config_name() function, into the original base package
  // configuration name (first) and the potentially empty configuration
  // variant name (second).
  //
  pair<string, string>
  split_package_build_config_name (const string&);

  // Convert the build-package-config-variant brep module options into the map
  // of the package configuration variant names to the newline separated list
  // of arguments, collected from the options specified for the same variant
  // name. Add an entry for the base package configuration (empty key),
  // potentially with an empty arguments list, if not specified explicitly in
  // brep configuration.
  //
  std::map<string, string>
  package_build_config_variants (const std::multimap<string, string>&);
}

#endif // MOD_BUILD_PACKAGE_CONFIG
