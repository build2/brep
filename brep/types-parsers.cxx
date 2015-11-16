// file      : brep/types-parsers.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/types-parsers>

#include <butl/path>

#include <brep/options>

using namespace butl;

namespace brep
{
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
      }
      catch (const invalid_path&)
      {
        throw invalid_value (o, v);
      }
    }

    void parser<dir_path>::
    parse (dir_path& x, scanner& s)
    {
      parse_path (x, s);
    }
  }
}
