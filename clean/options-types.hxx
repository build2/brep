// file      : clean/options-types.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef CLEAN_OPTIONS_TYPES_HXX
#define CLEAN_OPTIONS_TYPES_HXX

#include <map>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

namespace brep
{
  struct toolchain_timeouts: std::map<string, timestamp> {};
}

#endif // CLEAN_OPTIONS_TYPES_HXX
