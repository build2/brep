// file      : web/apache/request.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <strings.h> // strcasecmp()

#include <iomanip>
#include <sstream>
#include <cstring>
#include <cstdlib>

namespace web
{
  namespace apache
  {
    inline int request::
    flush ()
    {
      if (buffer_ && out_buf_)
      {
        auto b = dynamic_cast<std::stringbuf*> (out_buf_.get ());
        assert (b);

        std::string s (b->str ());

        if (!s.empty ())
        {
          // Before writing response read and discard request body if any.
          //
          int r = ap_discard_request_body (rec_);

          if (r == OK)
          {
            set_write_state ();

            if (ap_rwrite (s.c_str (), s.length (), rec_) < 0)
              rec_->status = HTTP_REQUEST_TIME_OUT;
          }

          else
            rec_->status = r;
        }

        out_.reset ();
        out_buf_.reset ();
      }

      return rec_->status == HTTP_OK || get_write_state () ? OK : rec_->status;
    }

    inline const request::string_ptr& request::
    form_data ()
    {
      if (!form_data_)
      {
        form_data_.reset (new std::string ());
        const char *ct = apr_table_get (rec_->headers_in, "Content-Type");

        if (ct && !strncasecmp ("application/x-www-form-urlencoded", ct, 33))
        {
          std::istream& istr (content ());
          std::getline (istr, *form_data_);

          // Make request data still be available.
          //

          std::unique_ptr<std::streambuf> in_buf (
            new std::stringbuf (*form_data_));

          in_.reset (new std::istream (in_buf.get ()));
          in_buf_ = std::move (in_buf);
          in_->exceptions (std::ios::failbit | std::ios::badbit);
        }
      }

      return form_data_;
    }

    inline void request::
    parse_parameters (const char* args)
    {
      for (auto n (args); n != 0; )
      {
        const char* v = std::strchr (n, '=');
        const char* e = ::strchr (n, '&');

        if (e && e < v)
          v = 0;

        std::string name (v
                          ? mime_url_decode (n, v) :
                          (e
                           ? mime_url_decode (n, e)
                           : mime_url_decode (n, n + std::strlen (n))));

        std::string value;

        if (v++)
        {
          value = e
            ? mime_url_decode (v, e)
            : mime_url_decode (v, v + std::strlen (v));
        }

        if (!name.empty () || !value.empty ())
          parameters_->emplace_back (std::move (name), std::move (value));

        n = e ? e + 1 : 0;
      }
    }

    inline void request::
    mime_url_encode (const char* v, std::ostream& o)
    {
      char f = o.fill ();
      std::ios_base::fmtflags g = o.flags ();
      o << std::hex << std::uppercase << std::right << std::setfill ('0');

      char c;

      while ((c = *v++) != '\0')
      {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9'))
        {
          o << c;
        }
        else
          switch (c)
          {
          case ' ': o << '+'; break;
          case '.':
          case '_':
          case '-':
          case '~': o << c; break;
          default:
            {
              o << "%" << std::setw (2) << static_cast<unsigned short> (c);
              break;
            }
          }
      }

      o.flags (g);
      o.fill (f);
    }

    inline std::string request::
    mime_url_decode (const char* b, const char* e, bool trim)
    {
      if (trim)
      {
        b += std::strspn (b, " ");

        if (b >= e)
          return std::string ();

        while (*--e == ' ');
        ++e;
      }

      std::string value;
      value.reserve (e - b);

      char bf[3];
      bf[2] = '\0';

      while (b != e)
      {
        char c = *b++;

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
              throw std::invalid_argument (
                "::web::apache::request::mime_url_decode short");
            }

            *bf = *b;
            bf[1] = b[1];

            char* ebf = 0;
            size_t vl = std::strtoul (bf, &ebf, 16);

            if (*ebf != '\0')
            {
              throw std::invalid_argument (
                "::web::apache::request::mime_url_decode wrong");
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
}
