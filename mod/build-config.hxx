// file      : mod/build-config.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_BUILD_CONFIG_HXX
#define MOD_BUILD_CONFIG_HXX

#include <map>

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

  // Match a build configuration against the name and target patterns.
  //
  bool
  match (const string& config_pattern,
         const optional<string>& target_pattern,
         const bbot::build_config&);

  // Return true if a package excludes the specified build configuration.
  //
  bool
  exclude (const build_package&, const bbot::build_config&);
}

#endif // MOD_BUILD_CONFIG_HXX
