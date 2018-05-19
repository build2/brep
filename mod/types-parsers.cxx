// file      : mod/types-parsers.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/types-parsers.hxx>

#include <mod/options.hxx>

using namespace std;
using namespace web::xhtml;

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

    void parser<path>::
    parse (path& x, bool& xs, scanner& s)
    {
      xs = true;
      parse_path (x, s);
    }

    void parser<dir_path>::
    parse (dir_path& x, bool& xs, scanner& s)
    {
      xs = true;
      parse_path (x, s);
    }

    // Parse page_form.
    //
    void parser<page_form>::
    parse (page_form& x, bool& xs, scanner& s)
    {
      xs = true;
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

    // Parse page_menu.
    //
    void parser<page_menu>::
    parse (page_menu& x, bool& xs, scanner& s)
    {
      xs = true;
      const char* o (s.next ());

      if (!s.more ())
        throw missing_value (o);

      const string v (s.next ());

      auto p (v.find ('='));
      if (p != string::npos)
      {
        string label (v, 0, p);
        string link (v, p + 1);

        if (!label.empty ())
        {
          x = page_menu (move (label), move (link));
          return;
        }
      }

      throw invalid_value (o, v);
    }

    // Parse web::xhtml::fragment.
    //
    void parser<fragment>::
    parse (fragment& x, bool& xs, scanner& s)
    {
      xs = true;
      const char* o (s.next ());

      if (!s.more ())
        throw missing_value (o);

      const char* v (s.next ());

      try
      {
        x = fragment (v, o);
      }
      catch (const xml::parsing&)
      {
        throw invalid_value (o, v);
      }
    }
  }
}
