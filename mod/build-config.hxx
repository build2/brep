// file      : mod/build-config.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_BUILD_CONFIG_HXX
#define MOD_BUILD_CONFIG_HXX

#include <libbbot/build-config.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <libbrep/build.hxx>

namespace brep
{
  // Return pointer to the shared build configurations instance, creating one
  // on the first call. Throw tab_parsing on parsing error, io_error on the
  // underlying OS error. Is not thread-safe.
  //
  shared_ptr<const bbot::build_configs>
  shared_build_config (const path&);

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
}

#endif // MOD_BUILD_CONFIG_HXX
