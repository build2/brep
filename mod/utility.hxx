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

  // Transform the wildcard to the `SIMILAR TO` pattern.
  //
  // Note that the empty wildcard is transformed to the '%' pattern.
  //
  string
  wildcard_to_similar_to_pattern (const string&);

  // Sleep for a random period of time before retrying an action after its
  // recoverable failure (for example, odb::recoverable exception). The
  // maximum sleep time is specified in milliseconds.
  //
  // Note that the current implementation doesn't sleep on the first retry
  // (retry argument is 0) yielding instead.
  //
  // Also note that in the future we may support growth of the sleep time for
  // greater retry numbers.
  //
  void
  sleep_before_retry (size_t retry, size_t max_time = 100);
}

#endif // MOD_UTILITY_HXX
