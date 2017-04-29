// file      : load/types-parsers.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

// CLI parsers, included into the generated source files.
//

#ifndef BREP_LOAD_TYPES_PARSERS_HXX
#define BREP_LOAD_TYPES_PARSERS_HXX

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

#endif // BREP_LOAD_TYPES_PARSERS_HXX
