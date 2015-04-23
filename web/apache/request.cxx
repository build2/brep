// file      : web/apache/request.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <web/apache/request>

#include <stdexcept>
#include <ios>
#include <streambuf>
#include <sstream>
#include <ostream>
#include <memory>    // unique_ptr
#include <algorithm> // move()
#include <chrono>
#include <ctime>

#include <strings.h> // strcasecmp()

#include <apr_tables.h>

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

              string name (
                v ? mime_url_decode (n, v, true) :
                (e ? mime_url_decode (n, e, true) :
                 mime_url_decode (n, n + strlen (n), true)));

              string value;

              if (v++)
              {
                value = e ? mime_url_decode (v, e, true) :
                  mime_url_decode (v, v + strlen (v), true);
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
      if (type.empty ())
      {
        // Getting content stream for writing assumes type to be provided.
        //
        throw std::invalid_argument (
          "::web::apache::request::content invalid type");
      }

      // Due to apache implementation of error custom response there is no
      // way to make it unbuffered.
      //
      buffer = buffer || status != HTTP_OK;

      if ((status != status_ || type != type_ || buffer != buffer_) &
          write_flag ())
      {
        throw sequence_error ("::web::apache::request::content");
      }

      if (status == status_ && type == type_ && buffer == buffer_)
      {
        assert (out_);
        return *out_;
      }

      if (!buffer)
        // Request body will be discarded prior first byte of content is
        // written. Save form data now to make it available for furture
        // parameters () call.
        //
        form_data ();

      std::unique_ptr<std::streambuf> out_buf(
        buffer ? static_cast<std::streambuf*> (new std::stringbuf ()) :
        static_cast<std::streambuf*> (new ostreambuf (rec_)));

      out_.reset (new std::ostream (out_buf.get ()));

      out_buf_ = std::move (out_buf);

      out_->exceptions (
        std::ios::eofbit | std::ios::failbit | std::ios::badbit);

      status_ = status;
      type_ = type;
      buffer_ = buffer;

      if (!buffer_)
        set_content_type ();

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
      if (write_flag ())
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
