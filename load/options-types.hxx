// file      : load/options-types.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LOAD_OPTIONS_TYPES_HXX
#define LOAD_OPTIONS_TYPES_HXX

#include <libbrep/types.hxx>

namespace brep
{
  // Ignore unresolved conditional dependencies.
  //
  enum class ignore_unresolved_conditional_dependencies
  {
    all,  // For all packages.
    tests // Only for external tests, examples, and benchmarks packages.
  };
}

#endif // LOAD_OPTIONS_TYPES_HXX
