// file      : web/apache/request.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef WEB_APACHE_REQUEST_HXX
#define WEB_APACHE_REQUEST_HXX

#include <httpd.h> // request_rec, HTTP_*, OK, M_POST

#include <chrono>
#include <memory>    // unique_ptr
#include <string>
#include <vector>
#include <istream>
#include <ostream>
#include <streambuf>

#include <web/module.hxx>
#include <web/apache/stream.hxx>

namespace web
{
  namespace apache
  {
    // The state of the request processing, reflecting an interaction with
    // Apache API (like reading/writing content function calls), with no
    // buffering taken into account. Any state different from the initial
    // suppose that some irrevocable interaction with Apache API have
    // happened, so request processing should be either completed, or
    // reported as failed. State values are ordered in a sense that the
    // higher value reflects the more advanced stage of processing, so the
    // request current state value may not decrease.
    //
    enum class request_state
    {
      // Denotes the initial stage of the request handling. At this stage
      // the request line and headers are already parsed by Apache.
      //
      initial,

      // Reading the request content.
      //
      reading,

      // Adding the response headers (cookies in particular).
      //
      headers,

      // Writing the response content.
      //
      writing
    };

    // Extends istreambuf with read limit checking, caching, etc. (see the
    // implementation for details).
    //
    class istreambuf_cache;

    // Stream type for reading from Apache's bucket brigades.
    //
    class istream_buckets;

    class request: public web::request,
                   public web::response,
                   public stream_state
    {
      friend class service;

      // Can not be inline/default due to the member of
      // unique_ptr<istreambuf_cache> type. Note that istreambuf_cache type is
      // incomplete.
      //
      request (request_rec* rec) noexcept;
      ~request ();

      request_state
      state () const noexcept {return state_;}

      // Flush the buffered response content if present. The returned value
      // should be passed to Apache API on request handler exit.
      //
      int
      flush ();

      // Prepare for the request re-processing if possible (no unbuffered
      // read/write operations have been done). Throw sequence_error
      // otherwise. In particular, the preparation can include the response
      // content buffer cleanup, the request content buffer rewind.
      //
      void
      rewind ();

      // Get request path.
      //
      virtual const path_type&
      path ();

      // Get request body data stream.
      //
      virtual std::istream&
      content (std::size_t limit = 0, std::size_t buffer = 0);

      // Get request parameters.
      //
      virtual const name_values&
      parameters (std::size_t limit, bool url_only = false);

      // Get upload stream.
      //
      virtual std::istream&
      open_upload (std::size_t index);

      virtual std::istream&
      open_upload (const std::string& name);

      // Get request headers.
      //
      virtual const name_values&
      headers ();

      // Get request cookies.
      //
      virtual const name_values&
      cookies ();

      // Get response status code.
      //
      status_code
      status () const noexcept {return rec_->status;}

      // Set response status code.
      //
      virtual void
      status (status_code status);

      // Set response status code, content type and get body stream.
      //
      virtual std::ostream&
      content (status_code status,
               const std::string& type,
               bool buffer = true);

      // Add response cookie.
      //
      virtual void
      cookie (const char* name,
              const char* value,
              const std::chrono::seconds* max_age = nullptr,
              const char* path = nullptr,
              const char* domain = nullptr,
              bool secure = false,
              bool buffer = true);

    private:
      // On the first call cache the application/x-www-form-urlencoded or
      // multipart/form-data form data for the subsequent parameters parsing
      // and set the multipart flag accordingly. Don't cache if the request is
      // in the reading or later state. Return true if the cache contains the
      // form data.
      //
      // Note that the function doesn't change the content buffering (see
      // content() function for details) nor rewind the content stream after
      // reading.
      //
      bool
      form_data (std::size_t limit);

      // Used to also parse application/x-www-form-urlencoded POST body.
      //
      void
      parse_url_parameters (const char* args);

      void
      parse_multipart_parameters (const std::vector<char>& body);

      // Return a list of the upload input streams. Throw sequence_error if
      // the parameters() function was not called yet. Throw invalid_argument
      // if the request doesn't contain multipart form data.
      //
      using uploads_type = std::vector<std::unique_ptr<istream_buckets>>;

      uploads_type&
      uploads () const;

      // Advance the request processing state. Noop if new state is equal to
      // the current one. Throw sequence_error if the new state is less then
      // the current one. Can throw invalid_request if HTTP request is
      // malformed.
      //
      void
      state (request_state);

      // stream_state members implementation.
      //
      virtual void
      set_read_state () {state (request_state::reading);}

      virtual void
      set_write_state () {state (request_state::writing);}

    private:
      request_rec* rec_;
      request_state state_ = request_state::initial;

      path_type path_;

      std::unique_ptr<name_values> parameters_;
      bool url_only_parameters_; // Meaningless if parameters_ is NULL;

      // Uploaded file streams. If not NULL, is parallel to the parameters
      // list.
      //
      std::unique_ptr<uploads_type> uploads_;

      std::unique_ptr<name_values> headers_;
      std::unique_ptr<name_values> cookies_;

      // Form data cache. Is empty if the body doesn't contain the form data.
      //
      std::unique_ptr<std::vector<char>> form_data_;
      bool form_multipart_; // Meaningless if form_data_ is NULL or empty;

      std::unique_ptr<istreambuf_cache> in_buf_;
      std::unique_ptr<std::istream> in_;

      std::unique_ptr<std::streambuf> out_buf_;
      std::unique_ptr<std::ostream> out_;
    };
  }
}

#include <web/apache/request.ixx>

#endif // WEB_APACHE_REQUEST_HXX
