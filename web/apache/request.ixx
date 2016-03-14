// file      : web/apache/request.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <strings.h> // strncasecmp()

#include <apr_tables.h>  // apr_table_*

#include <http_protocol.h> // ap_*()

#include <sstream>
#include <utility> // move()

namespace web
{
  namespace apache
  {
    inline int request::
    flush ()
    {
      if (std::stringbuf* b = dynamic_cast<std::stringbuf*> (out_buf_.get ()))
      {
        // Response content is buffered.
        //
        std::string s (b->str ());

        if (!s.empty ())
        {
          try
          {
            state (request_state::writing);

            if (ap_rwrite (s.c_str (), s.length (), rec_) < 0)
              rec_->status = HTTP_REQUEST_TIME_OUT;
          }
          catch (const invalid_request& e)
          {
            rec_->status = e.status;
          }
        }

        out_.reset ();
        out_buf_.reset ();
      }

      return rec_->status == HTTP_OK || state_ >= request_state::writing
        ? OK
        : rec_->status;
    }

    inline const std::string& request::
    form_data ()
    {
      if (!form_data_)
      {
        form_data_.reset (new std::string ());

        if (rec_->method_number == M_POST)
        {
          const char* ct (apr_table_get (rec_->headers_in, "Content-Type"));

          if (ct != nullptr &&
              strncasecmp ("application/x-www-form-urlencoded", ct, 33) == 0)
          {
            std::istream& istr (content ());

            // Do not throw when eofbit is set (end of stream reached), and
            // when failbit is set (getline() failed to extract any
            // character).
            //
            istr.exceptions (std::istream::badbit);
            std::getline (istr, *form_data_);

            // Make this data the content of the input stream, so it's
            // available for the application as well.
            //
            std::unique_ptr<std::streambuf> in_buf (
              new std::stringbuf (*form_data_));

            in_.reset (new std::istream (in_buf.get ()));
            in_buf_ = std::move (in_buf);
            in_->exceptions (std::istream::failbit | std::istream::badbit);
          }
        }
      }

      return *form_data_;
    }
  }
}
