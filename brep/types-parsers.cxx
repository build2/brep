// file      : brep/types-parsers.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/types-parsers>

#include <brep/options>

using namespace std;

namespace brep
{
  namespace cli
  {
    // Parse path.
    //
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

    // Parse page_form.
    //
    void parser<page_form>::
    parse (page_form& x, scanner& s)
    {
      const char* o (s.next ());

      if (!s.more ())
        throw missing_value (o);

      const string v (s.next ());
      if (v == "full")
        x = page_form::full;
      else if (v == "brief")
        x = page_form::brief;
      else
        throw invalid_value (o, v);
    }
  }
}
