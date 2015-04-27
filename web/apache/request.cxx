// file      : web/apache/request.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <web/apache/request>

#include <apr_tables.h>

#include <strings.h> // strcasecmp()

#include <ios>
#include <ctime>
#include <chrono>
#include <memory>    // unique_ptr
#include <sstream>
#include <ostream>
#include <cstring>
#include <stdexcept>
#include <streambuf>
#include <algorithm> // move()

using namespace std;

namespace web
{
  namespace apache
  {
    const name_values& request::
    cookies ()
    {
      if (!cookies_)
      {
        cookies_.reset (new name_values ());

        const apr_array_header_t* ha = apr_table_elts (rec_->headers_in);
        size_t n = ha->nelts;

        for (auto h (reinterpret_cast<const apr_table_entry_t *> (ha->elts));
             n--; ++h)
        {
          if (!::strcasecmp (h->key, "Cookie"))
          {
            for (const char* n (h->val); n != 0; )
            {
              const char* v = strchr (n, '=');
              const char* e = strchr (n, ';');

              if (e && e < v)
                v = 0;

              string name (v
                           ? mime_url_decode (n, v, true)
                           : (e
                              ? mime_url_decode (n, e, true)
                              : mime_url_decode (n, n + strlen (n), true)));

              string value;

              if (v++)
              {
                value = e
                  ? mime_url_decode (v, e, true)
                  : mime_url_decode (v, v + strlen (v), true);
              }

              if (!name.empty () || !value.empty ())
                cookies_->emplace_back (move (name), move (value));

              n = e ? e + 1 : 0;
            }
          }
        }
      }

      return *cookies_;
    }

    ostream& request::
    content (status_code status, const std::string& type, bool buffer)
    {
      if (out_ && status == rec_->status && buffer == buffer_ &&
          !::strcasecmp (rec_->content_type ? rec_->content_type : "",
                         type.c_str ()))
      {
        return *out_;
      }

      if (get_write_state ())
      {
        throw sequence_error ("::web::apache::request::content");
      }

      if (!buffer)
        // Request body will be discarded prior first byte of content is
        // written. Save form data now to make it available for furture
        // parameters () call.
        //
        form_data ();

      std::unique_ptr<std::streambuf> out_buf (
        buffer
        ? static_cast<std::streambuf*> (new std::stringbuf ())
        : static_cast<std::streambuf*> (new ostreambuf (rec_, *this)));

      out_.reset (new std::ostream (out_buf.get ()));

      out_buf_ = std::move (out_buf);

      out_->exceptions (
        std::ios::eofbit | std::ios::failbit | std::ios::badbit);

      buffer_ = buffer;
      rec_->status = status;

      ap_set_content_type (
        rec_,
        type.empty () ? nullptr : apr_pstrdup (rec_->pool, type.c_str ()));

      return *out_;
    }

    void request::
    cookie (const char* name,
            const char* value,
            const std::chrono::seconds* max_age,
            const char* path,
            const char* domain,
            bool secure)
    {
      if (get_write_state ())
      {
        throw sequence_error ("::web::apache::request::cookie");
      }

      std::ostringstream s;
      mime_url_encode (name, s);
      s << "=";
      mime_url_encode (value, s);

      if (max_age)
      {
        std::chrono::system_clock::time_point tp =
          std::chrono::system_clock::now () + *max_age;

        std::time_t t = std::chrono::system_clock::to_time_t (tp);

        // Assume global "C" locale is not changed.
        //
        char b[100];
        std::strftime (b,
                       sizeof (b),
                       "%a, %d-%b-%Y %H:%M:%S GMT",
                       std::gmtime (&t));

        s << "; Expires=" << b;
      }

      if (path)
      {
        s << ";Path=" << path;
      }

      if (domain)
      {
        s << ";Domain=" << domain;
      }

      if (secure)
      {
        s << ";Secure";
      }

      apr_table_add (rec_->err_headers_out, "Set-Cookie", s.str ().c_str ());
    }

  }
}
