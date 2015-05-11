// file      : web/apache/request.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
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
        auto b (dynamic_cast<std::stringbuf*> (out_buf_.get ()));
        assert (b);

        std::string s (b->str ());

        if (!s.empty ())
        {
          // Before writing response read and discard request body if any.
          //
          int r (ap_discard_request_body (rec_));

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

    inline const std::string& request::
    form_data ()
    {
      if (!form_data_)
      {
        form_data_.reset (new std::string ());
        const char* ct (apr_table_get (rec_->headers_in, "Content-Type"));

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

      return *form_data_;
    }
  }
}
