// file      : load/types-parsers.cxx -*- C++ -*-
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

  void parser<ignore_unresolved_conditional_dependencies>::
  parse (ignore_unresolved_conditional_dependencies& x, bool& xs, scanner& s)
  {
    xs = true;
    const char* o (s.next ());

    if (!s.more ())
      throw missing_value (o);

    const string v (s.next ());
    if (v == "all")
      x = ignore_unresolved_conditional_dependencies::all;
    else if (v == "tests")
      x = ignore_unresolved_conditional_dependencies::tests;
    else
      throw invalid_value (o, v);
  }
}
