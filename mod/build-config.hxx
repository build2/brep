// file      : mod/build-config.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_BUILD_CONFIG_HXX
#define MOD_BUILD_CONFIG_HXX

#include <libbbot/build-config.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

namespace brep
{
  // Return pointer to the shared build configurations instance, creating one
  // on the first call. Throw tab_parsing on parsing error, io_error on the
  // underlying OS error. Is not thread-safe.
  //
  shared_ptr<const bbot::build_configs>
  shared_build_config (const path&);
}

#endif // MOD_BUILD_CONFIG_HXX
