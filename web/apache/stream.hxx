// file      : web/apache/stream.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef WEB_APACHE_STREAM_HXX
#define WEB_APACHE_STREAM_HXX

#include <httpd.h>         // request_rec, HTTP_*
#include <http_protocol.h> // ap_*()

#include <ios>       // streamsize
#include <vector>
#include <cstring>   // memmove(), size_t
#include <streambuf>
#include <algorithm> // min(), max()

#include <web/module.hxx> // invalid_request

namespace web
{
  namespace apache
  {
    // Object of a class implementing this interface is intended for keeping
    // the state of communication with the client.
    //
    struct stream_state
    {
      // Called by istreambuf functions when content is about to be read from
      // the client. Can throw invalid_request or sequence_error.
      //
      virtual void
      set_read_state () = 0;

      // Called by ostreambuf functions when some content is about to be
      // written to the client. Can throw invalid_request or sequence_error.
      //
      virtual void
      set_write_state () = 0;
    };

    // Base class for ostreambuf and istreambuf. References request and
    // communication state structures.
    //
    class rbuf: public std::streambuf
    {
    protected:
      rbuf (request_rec* r, stream_state& s): rec_ (r), state_ (s) {}

    protected:
      request_rec* rec_;
      stream_state& state_;
    };

    class ostreambuf: public rbuf
    {
    public:
      ostreambuf (request_rec* r, stream_state& s): rbuf (r, s) {}

    private:
      virtual int_type
      overflow (int_type c)
      {
        if (c != traits_type::eof ())
        {
          state_.set_write_state ();

          char chr (c);

          // Throwing allows to distinguish comm failure from other IO error
          // conditions.
          //
          if (ap_rwrite (&chr, sizeof (chr), rec_) == -1)
            throw invalid_request (HTTP_REQUEST_TIME_OUT);
        }

        return c;
      }

      virtual std::streamsize
      xsputn (const char* s, std::streamsize num)
      {
        state_.set_write_state ();

        if (ap_rwrite (s, num, rec_) < 0)
          throw invalid_request (HTTP_REQUEST_TIME_OUT);

        return num;
      }

      virtual int
      sync ()
      {
        if (ap_rflush (rec_) < 0)
          throw invalid_request (HTTP_REQUEST_TIME_OUT);

        return 0;
      }
    };

    class istreambuf: public rbuf
    {
    public:
      istreambuf (request_rec* r,
                  stream_state& s,
                  size_t bufsize = 1024,
                  size_t putback = 1)
          : rbuf (r, s),
            bufsize_ (std::max (bufsize, (size_t)1)),
            putback_ (std::min (putback, bufsize_ - 1)),
            buf_ (bufsize_)
      {
        char* p (buf_.data () + putback_);
        setg (p, p, p);
      }

    protected:
      virtual int_type
      underflow ()
      {
        if (gptr () < egptr ())
          return traits_type::to_int_type (*gptr ());

        state_.set_read_state ();

        size_t pb (std::min ((size_t)(gptr () - eback ()), putback_));
        std::memmove (buf_.data () + putback_ - pb, gptr () - pb, pb);

        char* p (buf_.data () + putback_);
        int rb (ap_get_client_block (rec_, p, bufsize_ - putback_));

        if (rb == 0)
          return traits_type::eof ();

        if (rb < 0)
          throw invalid_request (HTTP_REQUEST_TIME_OUT);

        setg (p - pb, p, p + rb);
        return traits_type::to_int_type (*gptr ());
      }

    protected:
      size_t bufsize_;
      size_t putback_;
      std::vector<char> buf_;
    };
  }
}

#endif // WEB_APACHE_STREAM_HXX
