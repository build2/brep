// file      : mod/build.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_BUILD_HXX
#define MOD_BUILD_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <libbrep/build.hxx>

// Various package build-related utilities.
//
namespace brep
{
  // Return the package build log url. By default the url is to the operations
  // combined log.
  //
  string
  build_log_url (const string& host, const dir_path& root,
                 const build&,
                 const string* operation = nullptr);

  // Return the package build forced rebuild url.
  //
  string
  build_force_url (const string& host, const dir_path& root, const build&);
}

#endif // MOD_BUILD_HXX
