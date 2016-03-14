// file      : web/apache/request.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <web/apache/request>

#include <apr_tables.h>  // apr_table_*, apr_array_header_t
#include <apr_strings.h> // apr_pstrdup()

#include <httpd.h>         // request_rec, HTTP_*, OK
#include <http_protocol.h> // ap_*()

#include <strings.h> // strcasecmp()

#include <ctime>     // strftime(), time_t
#include <chrono>
#include <memory>    // unique_ptr
#include <string>
#include <cassert>
#include <sstream>
#include <ostream>
#include <istream>
#include <cstring>   // str*(), size_t
#include <utility>   // move()
#include <stdexcept> // invalid_argument
#include <exception> // current_exception()
#include <streambuf>

#include <butl/optional>

#include <web/mime-url-encoding>

using namespace std;

namespace web
{
  namespace apache
  {
    void request::
    state (request_state s)
    {
      assert (s != request_state::initial);

      if (s == state_)
        return; // Noop.

      if (s < state_)
      {
        // Can't "unwind" irrevocable interaction with Apache API.
        //
        static const char* names[] = {
          "initial", "reading", "headers", "writing"};

        string str ("web::apache::request::set_state: ");
        str += names[static_cast<size_t> (state_)];
        str += " to ";
        str += names[static_cast<size_t> (s)];

        throw sequence_error (move (str));
      }

      if (s == request_state::reading)
      {
        // Prepare request content for reading.
        //
        int r (ap_setup_client_block (rec_, REQUEST_CHUNKED_DECHUNK));

        if (r != OK)
          throw invalid_request (r);
      }
      else if (s > request_state::reading && state_ <= request_state::reading)
      {
        // Read request content if any, discard whatever is received.
        //
        int r (ap_discard_request_body (rec_));

        if (r != OK)
          throw invalid_request (r);
      }

      state_ = s;
    }

    void request::
    rewind ()
    {
      // @@ Request content buffering, and response cookies buffering are not
      //    supported yet. When done will be possible to rewind in broader
      //    range of cases.
      //

      if (state_ == request_state::initial ||

          // Form data have been read. Lucky case, can rewind.
          //
          (state_ == request_state::reading &&
           dynamic_cast<stringbuf*> (in_buf_.get ()) != nullptr))
      {
        out_.reset ();
        out_buf_.reset ();

        rec_->status = HTTP_OK;

        ap_set_content_type (rec_, nullptr);

        if (in_)
          in_->seekg (0);
      }
      else
        throw sequence_error ("web::apache::request::rewind");
    }

    istream& request::
    content (bool buffer)
    {
      assert (!buffer); // Request content buffering is not implemented yet.

      if (!in_)
      {
        unique_ptr<streambuf> in_buf (new istreambuf (rec_, *this));

        in_.reset (new istream (in_buf.get ()));
        in_buf_ = move (in_buf);
        in_->exceptions (istream::failbit | istream::badbit);

        // Save form data now otherwise will not be available to do later
        // when data already read from stream.
        //
        form_data ();
      }

      return *in_;
    }

    const path& request::
    path ()
    {
      if (path_.empty ())
      {
        path_ = path_type (rec_->uri);

        // Module request handler can not be called if URI is empty.
        //
        assert (!path_.empty ());
      }

      return path_;
    }

    const name_values& request::
    parameters ()
    {
      if (parameters_ == nullptr)
      {
        parameters_.reset (new name_values ());

        try
        {
          parse_parameters (rec_->args);
          parse_parameters (form_data ().c_str ());
        }
        catch (const invalid_argument& )
        {
          throw invalid_request ();
        }
      }

      return *parameters_;
    }

    const name_values& request::
    cookies ()
    {
      if (cookies_ == nullptr)
      {
        cookies_.reset (new name_values ());

        const apr_array_header_t* ha (apr_table_elts (rec_->headers_in));
        size_t n (ha->nelts);

        for (auto h (reinterpret_cast<const apr_table_entry_t *> (ha->elts));
             n--; ++h)
        {
          if (strcasecmp (h->key, "Cookie") == 0)
          {
            for (const char* n (h->val); n != nullptr; )
            {
              const char* v (strchr (n, '='));
              const char* e (strchr (n, ';'));

              if (e != nullptr && e < v)
                v = nullptr;

              string name (v != nullptr
                           ? mime_url_decode (n, v, true)
                           : (e
                              ? mime_url_decode (n, e, true)
                              : mime_url_decode (n, n + strlen (n), true)));

              optional<string> value;

              if (v++)
                value = e
                  ? mime_url_decode (v, e, true)
                  : mime_url_decode (v, v + strlen (v), true);

              if (!name.empty () || value)
                cookies_->emplace_back (move (name), move (value));

              n = e ? e + 1 : nullptr;
            }
          }
        }
      }

      return *cookies_;
    }

    ostream& request::
    content (status_code status, const string& type, bool buffer)
    {
      if (out_ &&

          // Same status code.
          //
          status == rec_->status &&

          // Same buffering flag.
          //
          buffer ==
          (dynamic_cast<stringbuf*> (out_buf_.get ()) != nullptr) &&

          // Same content type.
          //
          strcasecmp (rec_->content_type ? rec_->content_type : "",
                      type.c_str ()) == 0)
      {
        // No change, return the existing stream.
        //
        return *out_;
      }

      if (state_ >= request_state::writing)
        throw sequence_error ("web::apache::request::content");

      if (!buffer)
        // Request body will be discarded prior first byte of content is
        // written. Save form data now to make it available for furture
        // parameters () call.
        //
        form_data ();

      unique_ptr<streambuf> out_buf (
        buffer
        ? static_cast<streambuf*> (new stringbuf ())
        : static_cast<streambuf*> (new ostreambuf (rec_, *this)));

      out_.reset (new ostream (out_buf.get ()));
      out_buf_ = move (out_buf);
      out_->exceptions (ostream::eofbit | ostream::failbit | ostream::badbit);

      rec_->status = status;

      ap_set_content_type (
        rec_,
        type.empty () ? nullptr : apr_pstrdup (rec_->pool, type.c_str ()));

      return *out_;
    }

    void request::
    status (status_code status)
    {
      if (status != rec_->status)
      {
        // Setting status code in exception handler is a common usecase
        // where no sense to throw but still need to signal apache a
        // proper status code.
        //
        if (state_ >= request_state::writing && !current_exception ())
          throw sequence_error ("web::apache::request::status");

        rec_->status = status;
        out_.reset ();
        out_buf_.reset ();
        ap_set_content_type (rec_, nullptr);
      }
    }

    void request::
    cookie (const char* name,
            const char* value,
            const chrono::seconds* max_age,
            const char* path,
            const char* domain,
            bool secure,
            bool buffer)
    {
      assert (!buffer); // Cookie buffering is not implemented yet.

      ostringstream s;
      mime_url_encode (name, s);
      s << "=";
      mime_url_encode (value, s);

      if (max_age)
      {
        chrono::system_clock::time_point tp (
          chrono::system_clock::now () + *max_age);

        time_t t (chrono::system_clock::to_time_t (tp));

        // Assume global locale is not changed and still "C".
        //
        char b[100];
        strftime (b, sizeof (b), "%a, %d-%b-%Y %H:%M:%S GMT", gmtime (&t));
        s << "; Expires=" << b;
      }

      if (path)
        s << ";Path=" << path;

      if (domain)
        s << ";Domain=" << domain;

      if (secure)
        s << ";Secure";

      state (request_state::headers);
      apr_table_add (rec_->err_headers_out, "Set-Cookie", s.str ().c_str ());
    }

    void request::
    parse_parameters (const char* args)
    {
      for (auto n (args); n != nullptr; )
      {
        const char* v (strchr (n, '='));
        const char* e (strchr (n, '&'));

        if (e != nullptr && e < v)
          v = nullptr;

        string name (v != nullptr
                     ? mime_url_decode (n, v) :
                     (e
                      ? mime_url_decode (n, e)
                      : mime_url_decode (n, n + strlen (n))));

        optional<string> value;

        if (v++)
          value = e
            ? mime_url_decode (v, e)
            : mime_url_decode (v, v + strlen (v));

        if (!name.empty () || value)
          parameters_->emplace_back (move (name), move (value));

        n = e ? e + 1 : nullptr;
      }
    }
  }
}
