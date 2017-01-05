// file      : web/mime-url-encoding.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <web/mime-url-encoding>

#include <ios>       // hex, uppercase, right
#include <string>
#include <iomanip>   // setw(), setfill()
#include <ostream>
#include <sstream>
#include <cstring>   // size_t, strspn()
#include <stdexcept> // invalid_argument

using namespace std;

namespace web
{
  // Encode characters different from unreserved ones specified in
  // "2.3.  Unreserved Characters" of http://tools.ietf.org/html/rfc3986.
  //
  void
  mime_url_encode (const char* v, ostream& o)
  {
    char f (o.fill ());
    ostream::fmtflags g (o.flags ());
    o << hex << uppercase << right << setfill ('0');

    char c;
    while ((c = *v++) != '\0')
    {
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9'))
      {
        o << c;
      }
      else
      {
        switch (c)
        {
        case ' ': o << '+'; break;
        case '.':
        case '_':
        case '-':
        case '~': o << c; break;
        default:
          {
            o << "%" << setw (2) << static_cast<unsigned short> (c);
            break;
          }
        }
      }
    }

    o.flags (g);
    o.fill (f);
  }

  string
  mime_url_encode (const char* v)
  {
    stringstream o;
    mime_url_encode (v, o);
    return o.str ();
  }

  string
  mime_url_encode (const string& v)
  {
    return mime_url_encode (v.c_str ());
  }

  string
  mime_url_decode (const char* b, const char* e, bool trim)
  {
    if (trim)
    {
      b += strspn (b, " ");

      if (b >= e)
        return string ();

      while (*--e == ' ');
      ++e;
    }

    string value;
    value.reserve (e - b);

    char bf[3];
    bf[2] = '\0';

    while (b != e)
    {
      char c (*b++);
      switch (c)
      {
      case '+':
        {
          value.append (" ");
          break;
        }
      case '%':
        {
          if (*b == '\0' || b[1] == '\0')
          {
            throw invalid_argument ("::web::mime_url_decode short");
          }

          *bf = *b;
          bf[1] = b[1];

          char* ebf (nullptr);
          size_t vl (strtoul (bf, &ebf, 16));

          if (*ebf != '\0')
          {
            throw invalid_argument ("::web::mime_url_decode wrong");
          }

          value.append (1, static_cast<char> (vl));
          b += 2;
          break;
        }
      default:
        {
          value.append (1, c);
          break;
        }
      }
    }

    return value;
  }
}
