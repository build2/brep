// file      : web/server/apache/request.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <web/server/apache/request.hxx>

#include <apr.h>         // APR_SIZE_MAX
#include <apr_errno.h>   // apr_status_t, APR_SUCCESS, APR_E*, apr_strerror()
#include <apr_tables.h>  // apr_table_*, apr_table_*(), apr_array_header_t
#include <apr_strings.h> // apr_pstrdup()
#include <apr_buckets.h> // apr_bucket*, apr_bucket_*(), apr_brigade_*(),
                         // APR_BRIGADE_*()

#include <httpd.h>         // request_rec, HTTP_*, OK
#include <http_protocol.h> // ap_*()

#include <apreq2/apreq.h>        // APREQ_*
#include <apreq2/apreq_util.h>   // apreq_brigade_copy()
#include <apreq2/apreq_param.h>  // apreq_param_t, apreq_value_to_param()
#include <apreq2/apreq_parser.h> // apreq_parser_t, apreq_parser_make()

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
#include <iterator>  // istreambuf_iterator
#include <stdexcept> // invalid_argument, runtime_error
#include <exception> // current_exception()
#include <streambuf>
#include <algorithm> // min()

#include <libbutl/utility.hxx>   // icasecmp()
#include <libbutl/optional.hxx>
#include <libbutl/timestamp.hxx>

#include <web/server/mime-url-encoding.hxx>

using namespace std;
using namespace butl;

namespace web
{
  namespace apache
  {
    [[noreturn]] static void
    throw_internal_error (apr_status_t s, const string& what)
    {
      char buf[1024];
      throw runtime_error (what + ": " + apr_strerror (s, buf, sizeof (buf)));
    }

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

        // Bail out if the end of stream is reached.
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

    // Stream interface for reading from the Apache's bucket brigade. Put back
    // is not supported.
    //
    // Note that reading from a brigade bucket modifies the brigade in the
    // general case. For example, reading from a file bucket adds a new heap
    // bucket before the file bucket on every read. Traversing/reading through
    // such a bucket brigade effectively loads the whole file into the memory,
    // so the subsequent brigade traversal results in iterating over the
    // loaded heap buckets.
    //
    // To avoid such a behavior we will make a shallow copy of the original
    // bucket brigade, initially and for each rewind. Then, instead of
    // iterating, we will always read from the first bucket removing it after
    // the use.
    //
    class istreambuf_buckets: public streambuf
    {
    public:
      // The bucket brigade must exist during the object's lifetime.
      //
      explicit
      istreambuf_buckets (const apr_bucket_brigade* bs)
          : orig_buckets_ (bs),
            buckets_ (apr_brigade_create (bs->p, bs->bucket_alloc))

      {
        if (buckets_ == nullptr)
          throw_internal_error (APR_ENOMEM, "apr_brigade_create");

        rewind (); // Copy the original buckets.
      }

      void
      rewind ()
      {
        // Note that apreq_brigade_copy() appends buckets to the destination,
        // so we clean it up first.
        //
        apr_status_t r (apr_brigade_cleanup (buckets_.get ()));
        if (r != APR_SUCCESS)
          throw_internal_error (r, "apr_brigade_cleanup");

        r = apreq_brigade_copy (
          buckets_.get (),
          const_cast<apr_bucket_brigade*> (orig_buckets_));

        if (r != APR_SUCCESS)
          throw_internal_error (r, "apreq_brigade_copy");

        setg (nullptr, nullptr, nullptr);
      }

    private:
      virtual int_type
      underflow ()
      {
        if (gptr () < egptr ())
          return traits_type::to_int_type (*gptr ());

        // If the get-pointer is not NULL then it points to the data referred
        // by the first brigade bucket. As we will bail out or rewrite such a
        // pointer now there is no need for the bucket either, so we can
        // safely delete it.
        //
        if (gptr () != nullptr)
        {
          assert (!APR_BRIGADE_EMPTY (buckets_));

          // Note that apr_bucket_delete() is a macro and the following
          // call ends up badly (with SIGSEGV).
          //
          // apr_bucket_delete (APR_BRIGADE_FIRST (buckets_));
          //
          apr_bucket* b (APR_BRIGADE_FIRST (buckets_));
          apr_bucket_delete (b);
        }

        if (APR_BRIGADE_EMPTY (buckets_))
          return traits_type::eof ();

        apr_size_t n;
        const char* d;
        apr_bucket* b (APR_BRIGADE_FIRST (buckets_));
        apr_status_t r (apr_bucket_read (b, &d, &n, APR_BLOCK_READ));

        if (r != APR_SUCCESS)
          throw_internal_error (r, "apr_bucket_read");

        char* p (const_cast<char*> (d));
        setg (p, p, p + n);
        return traits_type::to_int_type (*gptr ());
      }

    private:
      const apr_bucket_brigade* orig_buckets_;

      struct brigade_deleter
      {
        void operator() (apr_bucket_brigade* p) const
        {
          if (p != nullptr)
          {
            apr_status_t r (apr_brigade_destroy (p));

            // Shouldn't fail unless something is severely damaged.
            //
            assert (r == APR_SUCCESS);
          }
        }
      };

      unique_ptr<apr_bucket_brigade, brigade_deleter> buckets_;
    };

    class istream_buckets_base
    {
    public:
      explicit
      istream_buckets_base (const apr_bucket_brigade* bs): buf_ (bs) {}

    protected:
      istreambuf_buckets buf_;
    };

    class istream_buckets: public istream_buckets_base, public istream
    {
    public:
      explicit
      istream_buckets (const apr_bucket_brigade* bs)
          // Note that calling dtor for istream object before init() is called
          // is undefined behavior. That's the reason for inventing the
          // istream_buckets_base class.
          //
          : istream_buckets_base (bs), istream (&buf_)
      {
        exceptions (failbit | badbit);
      }

      void
      rewind ()
      {
        buf_.rewind ();
        clear ();       // Clears *bit flags (in particular eofbit).
      }
    };

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

      // We don't need to rewind the input stream (which well may fail if
      // unbuffered) if the form data is already read.
      //
      if (in_ != nullptr && form_data_ == nullptr)
      {
        assert (in_buf_ != nullptr);

        in_buf_->rewind (); // Throws if impossible to rewind.
        in_->clear ();      // Clears *bit flags (in particular eofbit).
      }

      // Rewind uploaded file streams.
      //
      if (uploads_ != nullptr)
      {
        for (const unique_ptr<istream_buckets>& is: *uploads_)
        {
          if (is != nullptr)
            is->rewind ();
        }
      }
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
    parameters (size_t limit, bool url_only)
    {
      if (parameters_ == nullptr || url_only < url_only_parameters_)
      {
        try
        {
          if (parameters_ == nullptr)
          {
            parameters_.reset (new name_values ());
            parse_url_parameters (rec_->args);
          }

          if (!url_only && form_data (limit))
          {
            // After the form data is parsed we can clean it up for the
            // application/x-www-form-urlencoded encoding but not for the
            // multipart/form-data (see parse_multipart_parameters() for
            // details).
            //
            if (form_multipart_)
              parse_multipart_parameters (*form_data_);
            else
            {
              // Make the character vector a NULL-terminated string.
              //
              form_data_->push_back ('\0');

              parse_url_parameters (form_data_->data ());
              *form_data_ = vector<char> (); // Reset the cache.
            }
          }
        }
        catch (const invalid_argument&)
        {
          throw invalid_request ();
        }

        url_only_parameters_ = url_only;
      }

      return *parameters_;
    }

    bool request::
    form_data (size_t limit)
    {
      if (form_data_ == nullptr)
      {
        form_data_.reset (new vector<char> ());

        // We will not consider POST body as a form data if the request is in
        // the reading or later state.
        //
        if (rec_->method_number == M_POST && state_ < request_state::reading)
        {
          const char* ct (apr_table_get (rec_->headers_in, "Content-Type"));

          if (ct != nullptr)
          {
            form_multipart_ = icasecmp ("multipart/form-data", ct, 19) == 0;

            if (form_multipart_ ||
                icasecmp ("application/x-www-form-urlencoded", ct, 33) == 0)
              *form_data_ = vector<char> (
                istreambuf_iterator<char> (content (limit)),
                istreambuf_iterator<char> ());
          }
        }
      }

      return !form_data_->empty ();
    }

    void request::
    parse_url_parameters (const char* args)
    {
      assert (parameters_ != nullptr);

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

    void request::
    parse_multipart_parameters (const vector<char>& body)
    {
      assert (parameters_ != nullptr && uploads_ == nullptr);

      auto throw_bad_request = [] (apr_status_t s,
                                   status_code sc = HTTP_BAD_REQUEST)
      {
        char buf[1024];
        throw invalid_request (sc, apr_strerror (s, buf, sizeof (buf)));
      };

      // Create the file upload stream list, filling it with NULLs for the
      // parameters parsed from the URL query part.
      //
      uploads_.reset (
        new vector<unique_ptr<istream_buckets>> (parameters_->size ()));

      // All the required objects (parser, input/output buckets, etc.) will be
      // allocated in the request memory pool and so will have the HTTP
      // request duration lifetime.
      //
      apr_pool_t* pool (rec_->pool);

      // Create the input bucket brigade containing a single bucket that
      // references the form data.
      //
      apr_bucket_alloc_t* ba (apr_bucket_alloc_create (pool));
      if (ba == nullptr)
        throw_internal_error (APR_ENOMEM, "apr_bucket_alloc_create");

      apr_bucket_brigade* bb (apr_brigade_create (pool, ba));
      if (bb == nullptr)
        throw_internal_error (APR_ENOMEM, "apr_brigade_create");

      apr_bucket* b (
        apr_bucket_immortal_create (body.data (), body.size (), ba));

      if (b == nullptr)
        throw_internal_error (APR_ENOMEM, "apr_bucket_immortal_create");

      APR_BRIGADE_INSERT_TAIL (bb, b);

      if ((b = apr_bucket_eos_create (ba)) == nullptr)
        throw_internal_error (APR_ENOMEM, "apr_bucket_eos_create");

      APR_BRIGADE_INSERT_TAIL (bb, b);

      // Make sure that the parser will not swap the parsed data to disk
      // passing the maximum possible value for the brigade limit. This way
      // the resulting buckets will reference the form data directly, making
      // no copies. This why we should not reset the form data cache after
      // the parsing.
      //
      // Note that in future we may possibly setup the parser to read from the
      // Apache internals directly and enable swapping the data to disk to
      // minimize memory consumption.
      //
      apreq_parser_t* parser (
        apreq_parser_make (pool,
                           ba,
                           apr_table_get (rec_->headers_in, "Content-Type"),
                           apreq_parse_multipart,
                           APR_SIZE_MAX /* brigade_limit */,
                           nullptr /* temp_dir */,
                           nullptr /* hook */,
                           nullptr /* ctx */));

      if (parser == nullptr)
        throw_internal_error (APR_ENOMEM, "apreq_parser_make");

      // Create the output table that will be filled with the parsed
      // parameters.
      //
      apr_table_t* params (apr_table_make (pool, APREQ_DEFAULT_NELTS));
      if (params == nullptr)
        throw_internal_error (APR_ENOMEM, "apr_table_make");

      // Parse the form data.
      //
      apr_status_t r (apreq_parser_run (parser, params, bb));
      if (r != APR_SUCCESS)
        throw_bad_request (r);

      // Fill the parameter and file upload stream lists.
      //
      const apr_array_header_t* ps (apr_table_elts (params));
      size_t n (ps->nelts);

      for (auto p (reinterpret_cast<const apr_table_entry_t*> (ps->elts));
           n--; ++p)
      {
        assert (p->key != nullptr && p->val != nullptr);

        if (*p->key != '\0')
        {
          parameters_->emplace_back (p->key, optional<string> (p->val));

          const apreq_param_t* ap (apreq_value_to_param (p->val));
          assert (ap != nullptr); // Must always be resolvable.

          uploads_->emplace_back (ap->upload != nullptr
                                  ? new istream_buckets (ap->upload)
                                  : nullptr);
        }
      }
    }

    request::uploads_type& request::
    uploads () const
    {
      if (parameters_ == nullptr || url_only_parameters_)
        sequence_error ("web::apache::request::uploads");

      if (uploads_ == nullptr)
        throw invalid_argument ("no uploads");

      assert (uploads_->size () == parameters_->size ());
      return *uploads_;
    }

    istream& request::
    open_upload (size_t index)
    {
      uploads_type& us (uploads ());
      size_t n (us.size ());

      if (index >= n)
        throw invalid_argument ("invalid index");

      const unique_ptr<istream_buckets>& is (us[index]);

      if (is == nullptr)
        throw invalid_argument ("no upload");

      return *is;
    }

    istream& request::
    open_upload (const string& name)
    {
      uploads_type& us (uploads ());
      size_t n (us.size ());

      istream* r (nullptr);
      for (size_t i (0); i < n; ++i)
      {
        if ((*parameters_)[i].name == name)
        {
          istream* is (us[i].get ());

          if (is != nullptr)
          {
            if (r != nullptr)
              throw invalid_argument ("multiple uploads for '" + name + '\'');

            r = is;
          }
        }
      }

      if (r == nullptr)
        throw invalid_argument ("no upload");

      return *r;
    }

    const name_values& request::
    headers ()
    {
      if (headers_ == nullptr)
      {
        headers_.reset (new name_values ());

        const apr_array_header_t* ha (apr_table_elts (rec_->headers_in));
        size_t n (ha->nelts);

        headers_->reserve (n + 1); // One for the custom :Client-IP header.

        auto add = [this] (const char* n, const char* v)
        {
          assert (n != nullptr && v != nullptr);
          headers_->emplace_back (n, optional<string> (v));
        };

        for (auto h (reinterpret_cast<const apr_table_entry_t*> (ha->elts));
             n--; ++h)
          add (h->key, h->val);

        assert (rec_->connection != nullptr);

        add (":Client-IP", rec_->connection->client_ip);
      }

      return *headers_;
    }

    const name_values& request::
    cookies ()
    {
      if (cookies_ == nullptr)
      {
        cookies_.reset (new name_values ());

        const apr_array_header_t* ha (apr_table_elts (rec_->headers_in));
        size_t n (ha->nelts);

        for (auto h (reinterpret_cast<const apr_table_entry_t*> (ha->elts));
             n--; ++h)
        {
          assert (h->key != nullptr);

          if (icasecmp (h->key, "Cookie") == 0)
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
          icasecmp (type, rec_->content_type ? rec_->content_type : "") == 0)
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
        // In the rare cases when the form data is expectedly bigger than 64K
        // the client can always call parameters(limit) explicitly.
        //
        form_data (64 * 1024);

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
  }
}
