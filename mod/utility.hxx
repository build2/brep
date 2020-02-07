// file      : mod/utility.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_UTILITY_HXX
#define MOD_UTILITY_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

namespace brep
{
  // Append the @<tenant> leaf component to the directory if the tenant is
  // not empty. Otherwise, return the directory unchanged.
  //
  inline dir_path
  tenant_dir (const dir_path& dir, const string& tenant)
  {
    return !tenant.empty ()
           ? path_cast<dir_path> (dir / ('@' + tenant))
           : dir;
  }
}

#endif // MOD_UTILITY_HXX
