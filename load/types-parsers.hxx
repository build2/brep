// file      : load/types-parsers.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

// CLI parsers, included into the generated source files.
//

#ifndef LOAD_TYPES_PARSERS_HXX
#define LOAD_TYPES_PARSERS_HXX

#include <libbrep/types.hxx>

#include <load/options-types.hxx>

namespace cli
{
  class scanner;

  template <typename T>
  struct parser;

  template <>
  struct parser<brep::path>
  {
    static void
    parse (brep::path&, bool&, scanner&);
  };

  template <>
  struct parser<brep::ignore_unresolved_conditional_dependencies>
  {
    static void
    parse (brep::ignore_unresolved_conditional_dependencies&, bool&, scanner&);
  };
}

#endif // LOAD_TYPES_PARSERS_HXX
