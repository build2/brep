// -*- C++ -*-
//
// This file was generated by CLI, a command line interface
// compiler for C++.
//

// Begin prologue.
//
//
// End prologue.

#include <monitor/module-options.hxx>

#include <map>
#include <set>
#include <string>
#include <vector>
#include <utility>
#include <ostream>
#include <sstream>
#include <cstring>

namespace brep
{
  namespace cli
  {
    template <typename X>
    struct parser
    {
      static void
      parse (X& x, bool& xs, scanner& s)
      {
        using namespace std;

        const char* o (s.next ());
        if (s.more ())
        {
          string v (s.next ());
          istringstream is (v);
          if (!(is >> x && is.peek () == istringstream::traits_type::eof ()))
            throw invalid_value (o, v);
        }
        else
          throw missing_value (o);

        xs = true;
      }
    };

    template <>
    struct parser<bool>
    {
      static void
      parse (bool& x, bool& xs, scanner& s)
      {
        const char* o (s.next ());

        if (s.more ())
        {
          const char* v (s.next ());

          if (std::strcmp (v, "1")    == 0 ||
              std::strcmp (v, "true") == 0 ||
              std::strcmp (v, "TRUE") == 0 ||
              std::strcmp (v, "True") == 0)
            x = true;
          else if (std::strcmp (v, "0")     == 0 ||
                   std::strcmp (v, "false") == 0 ||
                   std::strcmp (v, "FALSE") == 0 ||
                   std::strcmp (v, "False") == 0)
            x = false;
          else
            throw invalid_value (o, v);
        }
        else
          throw missing_value (o);

        xs = true;
      }
    };

    template <>
    struct parser<std::string>
    {
      static void
      parse (std::string& x, bool& xs, scanner& s)
      {
        const char* o (s.next ());

        if (s.more ())
          x = s.next ();
        else
          throw missing_value (o);

        xs = true;
      }
    };

    template <typename X>
    struct parser<std::pair<X, std::size_t> >
    {
      static void
      parse (std::pair<X, std::size_t>& x, bool& xs, scanner& s)
      {
        x.second = s.position ();
        parser<X>::parse (x.first, xs, s);
      }
    };

    template <typename X>
    struct parser<std::vector<X> >
    {
      static void
      parse (std::vector<X>& c, bool& xs, scanner& s)
      {
        X x;
        bool dummy;
        parser<X>::parse (x, dummy, s);
        c.push_back (x);
        xs = true;
      }
    };

    template <typename X, typename C>
    struct parser<std::set<X, C> >
    {
      static void
      parse (std::set<X, C>& c, bool& xs, scanner& s)
      {
        X x;
        bool dummy;
        parser<X>::parse (x, dummy, s);
        c.insert (x);
        xs = true;
      }
    };

    template <typename K, typename V, typename C>
    struct parser<std::map<K, V, C> >
    {
      static void
      parse (std::map<K, V, C>& m, bool& xs, scanner& s)
      {
        const char* o (s.next ());

        if (s.more ())
        {
          std::size_t pos (s.position ());
          std::string ov (s.next ());
          std::string::size_type p = ov.find ('=');

          K k = K ();
          V v = V ();
          std::string kstr (ov, 0, p);
          std::string vstr (ov, (p != std::string::npos ? p + 1 : ov.size ()));

          int ac (2);
          char* av[] =
          {
            const_cast<char*> (o),
            0
          };

          bool dummy;
          if (!kstr.empty ())
          {
            av[1] = const_cast<char*> (kstr.c_str ());
            argv_scanner s (0, ac, av, false, pos);
            parser<K>::parse (k, dummy, s);
          }

          if (!vstr.empty ())
          {
            av[1] = const_cast<char*> (vstr.c_str ());
            argv_scanner s (0, ac, av, false, pos);
            parser<V>::parse (v, dummy, s);
          }

          m[k] = v;
        }
        else
          throw missing_value (o);

        xs = true;
      }
    };

    template <typename K, typename V, typename C>
    struct parser<std::multimap<K, V, C> >
    {
      static void
      parse (std::multimap<K, V, C>& m, bool& xs, scanner& s)
      {
        const char* o (s.next ());

        if (s.more ())
        {
          std::size_t pos (s.position ());
          std::string ov (s.next ());
          std::string::size_type p = ov.find ('=');

          K k = K ();
          V v = V ();
          std::string kstr (ov, 0, p);
          std::string vstr (ov, (p != std::string::npos ? p + 1 : ov.size ()));

          int ac (2);
          char* av[] =
          {
            const_cast<char*> (o),
            0
          };

          bool dummy;
          if (!kstr.empty ())
          {
            av[1] = const_cast<char*> (kstr.c_str ());
            argv_scanner s (0, ac, av, false, pos);
            parser<K>::parse (k, dummy, s);
          }

          if (!vstr.empty ())
          {
            av[1] = const_cast<char*> (vstr.c_str ());
            argv_scanner s (0, ac, av, false, pos);
            parser<V>::parse (v, dummy, s);
          }

          m.insert (typename std::multimap<K, V, C>::value_type (k, v));
        }
        else
          throw missing_value (o);

        xs = true;
      }
    };

    template <typename X, typename T, T X::*M>
    void
    thunk (X& x, scanner& s)
    {
      parser<T>::parse (x.*M, s);
    }

    template <typename X, bool X::*M>
    void
    thunk (X& x, scanner& s)
    {
      s.next ();
      x.*M = true;
    }

    template <typename X, typename T, T X::*M, bool X::*S>
    void
    thunk (X& x, scanner& s)
    {
      parser<T>::parse (x.*M, x.*S, s);
    }
  }
}

#include <map>

namespace brep
{
  namespace options
  {
    // module
    //

    module::
    module ()
    {
    }

    bool module::
    parse (int& argc,
           char** argv,
           bool erase,
           ::brep::cli::unknown_mode opt,
           ::brep::cli::unknown_mode arg)
    {
      ::brep::cli::argv_scanner s (argc, argv, erase);
      bool r = _parse (s, opt, arg);
      return r;
    }

    bool module::
    parse (int start,
           int& argc,
           char** argv,
           bool erase,
           ::brep::cli::unknown_mode opt,
           ::brep::cli::unknown_mode arg)
    {
      ::brep::cli::argv_scanner s (start, argc, argv, erase);
      bool r = _parse (s, opt, arg);
      return r;
    }

    bool module::
    parse (int& argc,
           char** argv,
           int& end,
           bool erase,
           ::brep::cli::unknown_mode opt,
           ::brep::cli::unknown_mode arg)
    {
      ::brep::cli::argv_scanner s (argc, argv, erase);
      bool r = _parse (s, opt, arg);
      end = s.end ();
      return r;
    }

    bool module::
    parse (int start,
           int& argc,
           char** argv,
           int& end,
           bool erase,
           ::brep::cli::unknown_mode opt,
           ::brep::cli::unknown_mode arg)
    {
      ::brep::cli::argv_scanner s (start, argc, argv, erase);
      bool r = _parse (s, opt, arg);
      end = s.end ();
      return r;
    }

    bool module::
    parse (::brep::cli::scanner& s,
           ::brep::cli::unknown_mode opt,
           ::brep::cli::unknown_mode arg)
    {
      bool r = _parse (s, opt, arg);
      return r;
    }

    typedef
    std::map<std::string, void (*) (module&, ::brep::cli::scanner&)>
    _cli_module_map;

    static _cli_module_map _cli_module_map_;

    struct _cli_module_map_init
    {
      _cli_module_map_init ()
      {
      }
    };

    static _cli_module_map_init _cli_module_map_init_;

    bool module::
    _parse (const char* o, ::brep::cli::scanner& s)
    {
      _cli_module_map::const_iterator i (_cli_module_map_.find (o));

      if (i != _cli_module_map_.end ())
      {
        (*(i->second)) (*this, s);
        return true;
      }

      // build_task base
      //
      if (::brep::options::build_task::_parse (o, s))
        return true;

      return false;
    }

    bool module::
    _parse (::brep::cli::scanner& s,
            ::brep::cli::unknown_mode opt_mode,
            ::brep::cli::unknown_mode arg_mode)
    {
      // Can't skip combined flags (--no-combined-flags).
      //
      assert (opt_mode != ::brep::cli::unknown_mode::skip);

      bool r = false;
      bool opt = true;

      while (s.more ())
      {
        const char* o = s.peek ();

        if (std::strcmp (o, "--") == 0)
        {
          opt = false;
          s.skip ();
          r = true;
          continue;
        }

        if (opt)
        {
          if (_parse (o, s))
          {
            r = true;
            continue;
          }

          if (std::strncmp (o, "-", 1) == 0 && o[1] != '\0')
          {
            // Handle combined option values.
            //
            std::string co;
            if (const char* v = std::strchr (o, '='))
            {
              co.assign (o, 0, v - o);
              ++v;

              int ac (2);
              char* av[] =
              {
                const_cast<char*> (co.c_str ()),
                const_cast<char*> (v)
              };

              ::brep::cli::argv_scanner ns (0, ac, av);

              if (_parse (co.c_str (), ns))
              {
                // Parsed the option but not its value?
                //
                if (ns.end () != 2)
                  throw ::brep::cli::invalid_value (co, v);

                s.next ();
                r = true;
                continue;
              }
              else
              {
                // Set the unknown option and fall through.
                //
                o = co.c_str ();
              }
            }

            // Handle combined flags.
            //
            char cf[3];
            {
              const char* p = o + 1;
              for (; *p != '\0'; ++p)
              {
                if (!((*p >= 'a' && *p <= 'z') ||
                      (*p >= 'A' && *p <= 'Z') ||
                      (*p >= '0' && *p <= '9')))
                  break;
              }

              if (*p == '\0')
              {
                for (p = o + 1; *p != '\0'; ++p)
                {
                  std::strcpy (cf, "-");
                  cf[1] = *p;
                  cf[2] = '\0';

                  int ac (1);
                  char* av[] =
                  {
                    cf
                  };

                  ::brep::cli::argv_scanner ns (0, ac, av);

                  if (!_parse (cf, ns))
                    break;
                }

                if (*p == '\0')
                {
                  // All handled.
                  //
                  s.next ();
                  r = true;
                  continue;
                }
                else
                {
                  // Set the unknown option and fall through.
                  //
                  o = cf;
                }
              }
            }

            switch (opt_mode)
            {
              case ::brep::cli::unknown_mode::skip:
              {
                s.skip ();
                r = true;
                continue;
              }
              case ::brep::cli::unknown_mode::stop:
              {
                break;
              }
              case ::brep::cli::unknown_mode::fail:
              {
                throw ::brep::cli::unknown_option (o);
              }
            }

            break;
          }
        }

        switch (arg_mode)
        {
          case ::brep::cli::unknown_mode::skip:
          {
            s.skip ();
            r = true;
            continue;
          }
          case ::brep::cli::unknown_mode::stop:
          {
            break;
          }
          case ::brep::cli::unknown_mode::fail:
          {
            throw ::brep::cli::unknown_argument (o);
          }
        }

        break;
      }

      return r;
    }
  }
}

// Begin epilogue.
//
//
// End epilogue.

