// file      : brep/package-traits.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/package-traits>

#include <string>
#include <ostream>
#include <sstream>
#include <cstring> // memcpy

#include <odb/pgsql/traits.hxx>

using namespace std;

namespace odb
{
  namespace pgsql
  {
    static inline void
    to_pg_string (ostream& os, const string& s)
    {
      os << '"';

      for (auto c: s)
      {
        if (c == '\\' || c == '"')
          os << '\\';

        os << c;
      }

      os << '"';
    }

    // Convert C++ weighted_text struct to PostgreSQL weighted_text
    // composite type.
    //
    void value_traits<brep::weighted_text, id_string>::
    set_image (details::buffer& b,
               size_t& n,
               bool& is_null,
               const value_type& v)
    {
      is_null = v.a.empty () && v.b.empty () && v.c.empty () && v.d.empty ();

      if (!is_null)
      {
        ostringstream o;
        o << "(";
        to_pg_string (o, v.a);
        o << ",";
        to_pg_string (o, v.b);
        o << ",";
        to_pg_string (o, v.c);
        o << ",";
        to_pg_string (o, v.d);
        o << ")";

        const string& s (o.str ());
        n = s.size ();

        if (n > b.capacity ())
          b.capacity (n);

        memcpy (b.data (), s.c_str (), n);
      }
    }
  }
}
