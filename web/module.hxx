// file      : web/module.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef WEB_MODULE_HXX
#define WEB_MODULE_HXX

#include <map>
#include <string>
#include <vector>
#include <iosfwd>
#include <chrono>
#include <cstdint>   // uint16_t
#include <cstddef>   // size_t
#include <utility>   // move()
#include <stdexcept> // runtime_error

#include <libbutl/path.mxx>
#include <libbutl/optional.mxx>

namespace web
{
  using butl::optional;

  // HTTP status code.
  //
  // @@ Define some commonly used constants?
  //
  using status_code = std::uint16_t;

  // This exception is used to signal that the request is invalid
  // (4XX codes) rather than that it could not be processed (5XX).
  // By default 400 is returned, which means the request is malformed.
  //
  // If caught by the web server implementation, it will try to return
  // the specified status and content to the client, if possible.
  // It is, however, may not be possible if some unbuffered content has
  // already been written. The behavior in this case is implementation-
  // specific and may result in no indication of an error being sent to
  // the client.
  //
  struct invalid_request
  {
    status_code status;
    std::string content;
    std::string type;

    //@@ Maybe optional "try again" link?
    //
    invalid_request (status_code s = 400,
                     std::string c = "",
                     std::string t = "text/plain;charset=utf-8")
        : status (s), content (std::move (c)), type (std::move (t)) {}
  };

  // Exception indicating HTTP request/response sequencing error.
  // For example, trying to change the status code after some
  // content has already been written.
  //
  struct sequence_error: std::runtime_error
  {
    sequence_error (std::string d): std::runtime_error (std::move (d)) {}
  };

  // Map of handler configuration option names to the boolean flag indicating
  // whether the value is expected for the option.
  //
  using option_descriptions = std::map<std::string, bool>;

  struct name_value
  {
    // These should eventually become string_view's.
    //
    std::string name;
    optional<std::string> value;

    name_value () {}
    name_value (std::string n, optional<std::string> v)
        : name (std::move (n)), value (std::move (v)) {}
  };

  using name_values = std::vector<name_value>;
  using butl::path;

  class request
  {
  public:
    using path_type = web::path;

    virtual
    ~request () = default;

    // Corresponds to abs_path portion of HTTP URL as described in "3.2.2 HTTP
    // URL" of http://tools.ietf.org/html/rfc2616. Returns '/' if no abs_path
    // is present in URL.
    //
    virtual const path_type&
    path () = 0;

    //@@ Why not pass parameters directly? Lazy parsing?
    //@@ Why not have something like operator[] for lookup? Probably
    //   in name_values.
    //@@ Maybe parameter_list() and parameter_map()?
    //
    // Parse parameters from the URL query part and from the HTTP POST request
    // body for the application/x-www-form-urlencoded or multipart/form-data
    // content type. Optionally limit the amount of data read from the body
    // (see the content() function for the semantics). Throw invalid_request
    // if parameters decoding fails.
    //
    virtual const name_values&
    parameters (std::size_t limit, bool url_only = false) = 0;

    // Open the input stream for the upload corresponding to the specified
    // parameter index. Must be called after the parameters() function is
    // called, throw sequence_error if that's not the case. Throw
    // invalid_argument if the index doesn't have an upload (for example,
    // because the parameter is not <input type="file"/> form field).
    //
    // Note also that reopening the same upload (within the same retry)
    // returns the same stream reference.
    //
    virtual std::istream&
    open_upload (std::size_t index) = 0;

    // As above but specify the parameter by name. Throw invalid_argument if
    // there are multiple uploads for this parameter name.
    //
    virtual std::istream&
    open_upload (const std::string& name) = 0;

    // Request headers.
    //
    // The implementation may add custom pseudo-headers reflecting additional
    // request options. Such headers should start with ':'. If possible, the
    // implementation should add the following well-known pseudo-headers:
    //
    // :Client-IP - IP address of the connecting client.
    //
    virtual const name_values&
    headers () = 0;

    // Throw invalid_request if cookies are malformed.
    //
    virtual const name_values&
    cookies () = 0;

    // Get the stream to read the request content from. If the limit argument
    // is zero, then the content limit is left unchanged (unlimited initially).
    // Otherwise the requested limit is set, and the invalid_request exception
    // with the code 413 (payload too large) will be thrown when the specified
    // limit is reached while reading from the stream. If the buffer argument
    // is zero, then the buffer size is left unchanged (zero initially). If it
    // is impossible to increase the buffer size (because, for example, some
    // content is already read unbuffered), then the sequence_error is thrown.
    //
    // Note that unread input content is discarded when any unbuffered content
    // is written, and any attempt to read it will result in the
    // sequence_error exception being thrown.
    //
    virtual std::istream&
    content (std::size_t limit, std::size_t buffer = 0) = 0;
  };

  class response
  {
  public:
    virtual
    ~response () = default;

    // Set status code, content type, and get the stream to write
    // the content to. If the buffer argument is true (default),
    // then buffer the entire content before sending it as a
    // response. This allows us to change the status code in
    // case of an error.
    //
    // Specifically, if there is already content in the buffer
    // and the status code is changed, then the old content is
    // discarded. If the content was not buffered and the status
    // is changed, then the sequence_error exception is thrown.
    // If this exception leaves handler::handle(), then the
    // implementation shall terminate the response in a suitable
    // but unspecified manner. In particular, there is no guarantee
    // that the user will be notified of an error or observe the
    // new status.
    //
    virtual std::ostream&
    content (status_code code = 200,
             const std::string& type = "application/xhtml+xml;charset=utf-8",
             bool buffer = true) = 0;

    // Set status code without writing any content. On status change,
    // discard buffered content or throw sequence_error if content was
    // not buffered.
    //
    virtual void
    status (status_code) = 0;

    // Throw sequence_error if some unbuffered content has already
    // been written.
    //
    virtual void
    cookie (const char* name,
            const char* value,
            const std::chrono::seconds* max_age = nullptr,
            const char* path = nullptr,
            const char* domain = nullptr,
            bool secure = false,
            bool buffer = true) = 0;
  };

  // A web server logging backend. The handler can use it to log
  // diagnostics that is meant for the web server operator rather
  // than the user.
  //
  // The handler can cast this basic interface to the web server's
  // specific implementation that may provide a richer interface.
  //
  class log
  {
  public:
    virtual
    ~log () = default;

    virtual void
    write (const char* msg) = 0;
  };

  // The web server creates a new handler instance for each request
  // by copy-initializing it with the handler exemplar. This way we
  // achieve two things: we can freely use handler data members
  // without worrying about multi-threading issues and we
  // automatically get started with the initial state for each
  // request. If you really need to share some rw-data between
  // all the handlers, use static data members with appropriate
  // locking. See the <service> header in one of the web server
  // directories (e.g., apache/) if you need to see the code that
  // does this.
  //
  class handler
  {
  public:
    virtual
    ~handler () = default;

    // Description of configuration options supported by this handler. Note:
    // should be callable during static initialization.
    //
    virtual option_descriptions
    options () = 0;

    // During startup the web server calls this function on the handler
    // exemplar to log the handler version information. It is up to the web
    // server whether to call this function once per handler implementation
    // type. Therefore, it is expected that this function will log the same
    // information for all the handler exemplars.
    //
    virtual void
    version (log&) = 0;

    // During startup the web server calls this function on the handler
    // exemplar passing a list of configuration options. The place these
    // configuration options come from is implementation-specific (normally
    // a configuration file). The web server guarantees that only options
    // listed in the map returned by the options() function above can be
    // present. Any exception thrown by this function terminates the web
    // server.
    //
    virtual void
    init (const name_values&, log&) = 0;

    // Return false if decline to handle the request. If handling have been
    // declined after any unbuffered content has been written, then the
    // implementation shall terminate the response in a suitable but
    // unspecified manner.
    //
    // Throw retry if need to retry handling the request. The retry will
    // happen on the same instance of the handler and the implementation is
    // expected to "rewind" the request and response objects to their initial
    // state. This is only guaranteed to be possible if the relevant functions
    // in the request and response objects were called in buffered mode (the
    // buffer argument was true).
    //
    // Any exception other than retry and invalid_request described above that
    // leaves this function is treated by the web server implementation as an
    // internal server error (500). Similar to invalid_request, it will try to
    // return the status and description (obtained by calling what() on
    // std::exception) to the client, if possible. The description is assume
    // to be encoded in UTF-8. The implementation may provide a configuration
    // option to omit the description from the response, for security/privacy
    // reasons.
    //
    struct retry {};

    virtual bool
    handle (request&, response&, log&) = 0;
  };
}

#endif // WEB_MODULE_HXX
