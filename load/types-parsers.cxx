// file      : load/types-parsers.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <load/types-parsers.hxx>

#include <load/load-options.hxx> // cli namespace

using namespace brep;

namespace cli
{
  template <typename T>
  static void
  parse_path (T& x, scanner& s)
  {
    const char* o (s.next ());

    if (!s.more ())
      throw missing_value (o);

    const char* v (s.next ());

    try
    {
      x = T (v);

      if (x.empty ())
        throw invalid_value (o, v);
    }
    catch (const invalid_path&)
    {
      throw invalid_value (o, v);
    }
  }

  void parser<path>::
  parse (path& x, bool& xs, scanner& s)
  {
    xs = true;
    parse_path (x, s);
  }
}
