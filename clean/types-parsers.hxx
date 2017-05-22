// file      : clean/types-parsers.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

// CLI parsers, included into the generated source files.
//

#ifndef CLEAN_TYPES_PARSERS_HXX
#define CLEAN_TYPES_PARSERS_HXX

#include <clean/options-types.hxx>

namespace cli
{
  class scanner;

  template <typename T>
  struct parser;

  template <>
  struct parser<brep::toolchain_timeouts>
  {
    static void
    parse (brep::toolchain_timeouts&, bool&, scanner&);
  };
}

#endif // CLEAN_TYPES_PARSERS_HXX
