// file      : web/apache/request.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <web/apache/request.hxx>

#include <apr_tables.h>  // apr_table_*, apr_array_header_t
#include <apr_strings.h> // apr_pstrdup()

#include <httpd.h>         // request_rec, HTTP_*, OK
#include <http_protocol.h> // ap_*()

#include <strings.h> // strcasecmp(), strncasecmp()

#include <ctime>     // strftime(), time_t
#include <vector>
#include <chrono>
#include <memory>    // unique_ptr
#include <string>
#include <cassert>
#include <ostream>
#include <istream>
#include <cstring>   // str*(), memcpy(), size_t
#include <utility>   // move()
#include <stdexcept> // invalid_argument
#include <exception> // current_exception()
#include <streambuf>
#include <algorithm> // min()

#include <libbutl/optional.mxx>
#include <libbutl/timestamp.mxx>

#include <web/mime-url-encoding.hxx>

using namespace std;
using namespace butl;

namespace web
{
  namespace apache
  {
    // Extend the Apache stream with checking for the read limit and caching
    // the content if requested. Replay the cached content after rewind.
    //
    class istreambuf_cache: public istreambuf
    {
      enum class mode
      {
        cache,  // Read from Apache stream, save the read data into the cache.
        replay, // Read from the cache.
        proxy   // Read from Apache stream (don't save into the cache).
      };

    public:
      istreambuf_cache (size_t read_limit, size_t cache_limit,
                        request_rec* r,
                        stream_state& s,
                        size_t bufsize = 1024,
                        size_t putback = 1)
          : istreambuf (r, s, bufsize, putback),
            read_limit_ (read_limit),
            cache_limit_ (cache_limit)
      {
      }

      void
      rewind ()
      {
        // Fail if some content is already missed in the cache.
        //
        if (mode_ == mode::proxy)
          throw sequence_error (
            string ("web::apache::istreambuf_cache::rewind: ") +
            (cache_limit_ > 0
             ? "half-buffered"
             : "unbuffered"));

        mode_ = mode::replay;
        replay_pos_ = 0;
        setg (nullptr, nullptr, nullptr);
      }

      void
      limits (size_t read_limit, size_t cache_limit)
      {
        if (read_limit > 0)
          read_limit_ = read_limit;

        if (cache_limit > 0)
        {
          // We can not increase the cache limit if some content is already
          // missed in the cache.
          //
          if (cache_limit > cache_limit_ && mode_ == mode::proxy)
            throw sequence_error (
              "web::apache::istreambuf_cache::limits: unbuffered");

          cache_limit_ = cache_limit;
        }
      }

      size_t read_limit  () const noexcept {return read_limit_;}
      size_t cache_limit () const noexcept {return cache_limit_;}

    private:
      virtual int_type
      underflow ();

    private:
      // Limits
      //
      size_t read_limit_;
      size_t cache_limit_;

      // State
      //
      mode mode_ = mode::cache;
      size_t read_bytes_ = 0;
      bool eof_ = false;        // End of Apache stream is reached.

      // Cache
      //
      struct chunk
      {
        vector<char> data;
        size_t offset;

        chunk (vector<char>&& d, size_t o): data (move (d)), offset (o) {}

        // Make the type move constructible-only to avoid copying of chunks on
        // vector growth.
        //
        chunk (chunk&&) = default;
      };

      vector<chunk> cache_;
      size_t cache_size_ = 0;
      size_t replay_pos_ = 0;
    };

    istreambuf_cache::int_type istreambuf_cache::
    underflow ()
    {
      if (gptr () < egptr ())
        return traits_type::to_int_type (*gptr ());

      if (mode_ == mode::replay)
      {
        if (replay_pos_ < cache_.size ())
        {
          chunk& ch (cache_[replay_pos_++]);
          char*  p  (ch.data.data ());
          setg (p, p + ch.offset, p + ch.data.size ());
          return traits_type::to_int_type (*gptr ());
        }

        // No more data to replay, so switch to the cache mode. That includes
        // resetting eback, gptr and egptr, so they point into the istreambuf's
        // internal buffer. Putback area should also be restored.
        //
        mode_ = mode::cache;

        // Bailout if the end of stream is reached.
        //
        if (eof_)
          return traits_type::eof ();

        char* p (buf_.data () + putback_);
        size_t pb (0);

        // Restore putback area if there is any cached data. Thanks to
        // istreambuf, it's all in a single chunk.
        //
        if (!cache_.empty ())
        {
          chunk& ch (cache_.back ());
          pb = min (putback_, ch.data.size ());
          memcpy (p - pb, ch.data.data () + ch.data.size () - pb, pb);
        }

        setg (p - pb, p, p);
      }

      // Delegate reading to the base class in the cache or proxy modes, but
      // check for the read limit first.
      //
      if (read_limit_ && read_bytes_ >= read_limit_)
        throw invalid_request (HTTP_REQUEST_ENTITY_TOO_LARGE,
                               "payload too large");

      // Throws the sequence_error exception if some unbuffered content is
      // already written.
      //
      int_type r (istreambuf::underflow ());

      if (r == traits_type::eof ())
      {
        eof_ = true;
        return r;
      }

      // Increment the read bytes counter.
      //
      size_t rb (egptr () - gptr ());
      read_bytes_ += rb;

      // In the cache mode save the read data if the cache limit is not
      // reached, otherwise switch to the proxy mode.
      //
      if (mode_ == mode::cache)
      {
        // Not to complicate things we will copy the buffer into the cache
        // together with the putback area, which is OK as it usually takes a
        // small fraction of the buffer. By the same reason we will cache the
        // whole data read even though we can exceed the limits by
        // bufsize - putback - 1 bytes.
        //
        if (cache_size_ < cache_limit_)
        {
          chunk ch (vector<char> (eback (), egptr ()),
                    static_cast<size_t> (gptr () - eback ()));

          cache_.emplace_back (move (ch));
          cache_size_ += rb;
        }
        else
          mode_ = mode::proxy;
      }

      return r;
    }

    // request
    //
    request::
    request (request_rec* rec) noexcept
      : rec_ (rec)
    {
      rec_->status = HTTP_OK;
    }

    request::
    ~request ()
    {
    }

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
      // @@ Response cookies buffering is not supported yet. When done will be
      //    possible to rewind in broader range of cases.
      //
      if (state_ > request_state::reading)
        throw sequence_error ("web::apache::request::rewind: unbuffered");

      out_.reset ();
      out_buf_.reset ();

      rec_->status = HTTP_OK;

      ap_set_content_type (rec_, nullptr); // Unset the output content type.

      if (in_ != nullptr)
        rewind_istream ();
    }

    void request::
    rewind_istream ()
    {
      assert (in_buf_ != nullptr && in_ != nullptr);

      in_buf_->rewind (); // Throws if impossible to rewind.
      in_->clear ();      // Clears *bit flags (in particular eofbit).
    }

    istream& request::
    content (size_t limit, size_t buffer)
    {
      // Create the input stream/streambuf if not present, otherwise adjust the
      // limits.
      //
      if (in_ == nullptr)
      {
        unique_ptr<istreambuf_cache> in_buf (
          new istreambuf_cache (limit, buffer, rec_, *this));

        in_.reset (new istream (in_buf.get ()));
        in_buf_ = move (in_buf);
        in_->exceptions (istream::failbit | istream::badbit);

        // Save form data now otherwise will not be available to do later when
        // data is already read from stream.
        //
        form_data ();
      }
      else
      {
        assert (in_buf_ != nullptr);
        in_buf_->limits (limit, buffer);
      }

      return *in_;
    }

    const path& request::
    path ()
    {
      if (path_.empty ())
      {
        path_ = path_type (rec_->uri); // Is already URL-decoded.

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
        // written. Save form data now to make it available for future
        // parameters() call.
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

      string s (mime_url_encode (name));
      s += "=";
      s += mime_url_encode (value);

      if (max_age)
      {
        timestamp tp (system_clock::now () + *max_age);
        time_t t (system_clock::to_time_t (tp));

        // Assume global locale is not changed and still "C".
        //
        char b[100];
        strftime (b, sizeof (b), "%a, %d-%b-%Y %H:%M:%S GMT", gmtime (&t));
        s += "; Expires=";
        s += b;
      }

      if (path)
      {
        s += ";Path=";
        s += path;
      }

      if (domain)
      {
        s += ";Domain=";
        s += domain;
      }

      if (secure)
        s += ";Secure";

      state (request_state::headers);
      apr_table_add (rec_->err_headers_out, "Set-Cookie", s.c_str ());
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

    const string& request::
    form_data ()
    {
      if (!form_data_)
      {
        form_data_.reset (new string ());

        if (rec_->method_number == M_POST)
        {
          const char* ct (apr_table_get (rec_->headers_in, "Content-Type"));

          if (ct != nullptr &&
              strncasecmp ("application/x-www-form-urlencoded", ct, 33) == 0)
          {
            size_t limit  (0);
            bool   rewind (true);

            // Assign some reasonable (64K) input content read/cache limits if
            // not done explicitly yet (with the request::content() call).
            // Rewind afterwards unless the cache limit is set to zero.
            //
            if (in_buf_ == nullptr)
              limit = 64 * 1024;
            else
              rewind = in_buf_->cache_limit () > 0;

            istream& istr (content (limit, limit));

            // Do not throw when eofbit is set (end of stream reached), and
            // when failbit is set (getline() failed to extract any character).
            //
            istream::iostate e (istr.exceptions ()); // Save exception mask.
            istr.exceptions (istream::badbit);
            getline (istr, *form_data_);
            istr.exceptions (e);                     // Restore exception mask.

            // Rewind the stream unless no buffering were requested beforehand.
            //
            if (rewind)
              rewind_istream ();
          }
        }
      }

      return *form_data_;
    }
  }
}
