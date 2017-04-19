// file      : web/apache/request.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <http_protocol.h> // ap_*()

#include <sstream> // stringbuf

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
  }
}
