// file      : mod/build-config.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_BUILD_CONFIG_HXX
#define MOD_BUILD_CONFIG_HXX

#include <map>
#include <algorithm> // find()

#include <libbbot/build-config.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <libbrep/build.hxx>
#include <libbrep/build-package.hxx>

#include <mod/options.hxx>

// Various build-related state and utilities.
//
namespace brep
{
  using bbot::build_config;

  // Return pointer to the shared build configurations instance, creating one
  // on the first call. Throw tab_parsing on parsing error, io_error on the
  // underlying OS error. Is not thread-safe.
  //
  shared_ptr<const bbot::build_configs>
  shared_build_config (const path&);

  // Map of build bot agent public keys fingerprints to the key file paths.
  //
  using bot_agent_keys = std::map<string, path>;

  // Return pointer to the shared build bot agent public keys map, creating
  // one on the first call. Throw system_error on the underlying openssl or OS
  // error. Not thread-safe.
  //
  shared_ptr<const bot_agent_keys>
  shared_bot_agent_keys (const options::openssl_options&, const dir_path&);

  // Return the package configuration build log url. By default the url is to
  // the operations combined log.
  //
  string
  build_log_url (const string& host, const dir_path& root,
                 const build&,
                 const string* operation = nullptr);

  // Return the package configuration forced rebuild url.
  //
  string
  force_rebuild_url (const string& host, const dir_path& root, const build&);

  // Check if the configuration belongs to the specified class.
  //
  inline bool
  belongs (const build_config& cfg, const char* cls)
  {
    const strings& cs (cfg.classes);
    return find (cs.begin (), cs.end (), cls) != cs.end ();
  }

  // Return true if the specified build configuration is excluded by a package
  // based on its underlying build class set, build class expressions, and
  // build constraints, potentially extending the underlying set with the
  // special classes. Set the exclusion reason if requested.
  //
  bool
  exclude (const build_class_exprs&,
           const build_constraints&,
           const build_config&,
           string* reason = nullptr);

  inline bool
  exclude (const build_package& p, const build_config& c, string* r = nullptr)
  {
    return exclude (p.builds, p.constraints, c, r);
  }

  // Convert the build configuration name, target, machine name or their
  // pattern into path, replacing dashes with slashes and double stars with
  // the `*/**/*` substring for a subsequent match using our path_match()
  // functionality. Throw invalid_path if the resulting path is invalid.
  //
  // Note that it is assumed that the match_absent path match flag will be
  // used for matching for the double star replacement to make sense.
  //
  path
  from_build_config_name (const string&);
}

#endif // MOD_BUILD_CONFIG_HXX
