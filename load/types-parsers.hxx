// file      : load/types-parsers.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

// CLI parsers, included into the generated source files.
//

#ifndef LOAD_TYPES_PARSERS_HXX
#define LOAD_TYPES_PARSERS_HXX

#include <libbrep/types.hxx>

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
}

#endif // LOAD_TYPES_PARSERS_HXX
