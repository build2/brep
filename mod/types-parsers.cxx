// file      : mod/types-parsers.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/types-parsers.hxx>

#include <sstream>

#include <libbutl/regex.hxx>
#include <libbutl/timestamp.hxx> // from_string()

#include <mod/module-options.hxx>

using namespace std;
using namespace butl;
using namespace bpkg;
using namespace bbot;
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

    // Parse time of day.
    //
    void parser<duration>::
    parse (duration& x, bool& xs, scanner& s)
    {
      xs = true;

      const char* o (s.next ());

      if (!s.more ())
        throw missing_value (o);

      const char* v (s.next ());

      // To avoid the manual time of day parsing and validation, let's parse
      // it as the first Epoch day time and convert the result (timestamp) to
      // the time elapsed since Epoch (duration).
      //
      try
      {
        string t ("1970-01-01 ");
        t += v;

        x = from_string (t.c_str (),
                         "%Y-%m-%d %H:%M",
                         false /* local */).time_since_epoch ();
        return;
      }
      catch (const invalid_argument&) {}
      catch (const system_error&) {}

      throw invalid_value (o, v);
    }

    // Parse repository_location.
    //
    void parser<repository_location>::
    parse (repository_location& x, bool& xs, scanner& s)
    {
      xs = true;

      const char* o (s.next ());

      if (!s.more ())
        throw missing_value (o);

      const char* v (s.next ());

      try
      {
        x = repository_location (v);
      }
      catch (const invalid_argument&)
      {
        throw invalid_value (o, v);
      }
    }

    // Parse interactive_mode.
    //
    void parser<interactive_mode>::
    parse (interactive_mode& x, bool& xs, scanner& s)
    {
      xs = true;
      const char* o (s.next ());

      if (!s.more ())
        throw missing_value (o);

      const string v (s.next ());

      try
      {
        x = to_interactive_mode (v);
      }
      catch (const invalid_argument&)
      {
        throw invalid_value (o, v);
      }
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

    // Parse the '/regex/replacement/' string into the regex/replacement pair.
    //
    void parser<pair<std::regex, string>>::
    parse (pair<std::regex, string>& x, bool& xs, scanner& s)
    {
      xs = true;
      const char* o (s.next ());

      if (!s.more ())
        throw missing_value (o);

      const char* v (s.next ());

      try
      {
        x = regex_replace_parse (v);
      }
      catch (const invalid_argument& e)
      {
        throw invalid_value (o, v, e.what ());
      }
      catch (const regex_error& e)
      {
        // Sanitize the description.
        //
        ostringstream os;
        os << e;

        throw invalid_value (o, v, os.str ());
      }
    }

    // Parse build_order.
    //
    void parser<build_order>::
    parse (build_order& x, bool& xs, scanner& s)
    {
      xs = true;
      const char* o (s.next ());

      if (!s.more ())
        throw missing_value (o);

      const string v (s.next ());
      if (v == "stable")
        x = build_order::stable;
      else if (v == "random")
        x = build_order::random;
      else
        throw invalid_value (o, v);
    }
  }
}
