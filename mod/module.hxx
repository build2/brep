// file      : mod/module.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MODULE_HXX
#define MOD_MODULE_HXX

#include <web/module.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/options.hxx>
#include <mod/diagnostics.hxx>

namespace brep
{
  // Bring in commonly used names from the web namespace.
  //
  // @@ Maybe doing using namespace is the right way to handle this.
  //    There will, however, most likely be a conflict between
  //    web::module and our module. Or maybe not, need to try.
  //
  using web::status_code;
  using web::invalid_request;
  using web::sequence_error;
  using web::option_descriptions;
  using web::name_value;
  using web::name_values;
  using web::request;
  using web::response;
  using web::log;

  // This exception indicated a server error (5XX). In particular,
  // it is thrown by the fail diagnostics stream and is caught by the
  // module implementation where it is both logged as an error and
  // returned to the user with the 5XX status code.
  //
  struct server_error
  {
    diag_data data;

    server_error (diag_data&& d): data (move (d)) {}
  };

  // Every module member function that needs to produce any diagnostics
  // shall begin with:
  //
  // MODULE_DIAG;
  //
  // This will instantiate the fail, error, warn, info, and trace
  // diagnostics streams with the function's name.
  //
#define MODULE_DIAG                                                     \
  const fail_mark<server_error> fail (__PRETTY_FUNCTION__);             \
  const basic_mark error (severity::error,                              \
                          this->log_writer_,                            \
                          __PRETTY_FUNCTION__);                         \
  const basic_mark warn (severity::warning,                             \
                         this->log_writer_,                             \
                         __PRETTY_FUNCTION__);                          \
  const basic_mark info (severity::info,                                \
                         this->log_writer_,                             \
                         __PRETTY_FUNCTION__);                          \
  const basic_mark trace (severity::trace,                              \
                          this->log_writer_,                            \
                          __PRETTY_FUNCTION__)

  // Adaptation of the web::module to our needs.
  //
  class module: public web::module
  {
    // Diagnostics.
    //
  protected:
    // Trace verbosity level.
    //
    // 0 - tracing disabled.
    // 1 - brief information regarding irregular situations, which not being
    //     an error can be of some interest.
    // 2 - @@ TODO: document
    //
    // While uint8 is more than enough, use uint16 for the ease of printing.
    //
    uint16_t verb_ = 0;

    template <class F> void l1 (const F& f) const {if (verb_ >= 1) f ();}
    template <class F> void l2 (const F& f) const {if (verb_ >= 2) f ();}

    // Set to true when the module is successfully initialized.
    //
    bool initialized_ {false};

    // Implementation details.
    //
  protected:
    module ();
    module (const module& );

    static name_values
    filter (const name_values&, const option_descriptions&);

    static option_descriptions
    convert (const cli::options&);

    static void
    append (option_descriptions& dst, const cli::options& src);

    static void
    append (option_descriptions& dst, const option_descriptions& src);

    // Can be used by module implementation to parse HTTP request parameters.
    //
    class name_value_scanner: public cli::scanner
    {
    public:
      name_value_scanner (const name_values&) noexcept;

      virtual bool
      more ();

      virtual const char*
      peek ();

      virtual const char*
      next ();

      virtual void
      skip ();

    private:
      const name_values& name_values_;
      name_values::const_iterator i_;
      bool name_;
    };

  public:
    virtual const cli::options&
    cli_options () const = 0;

    virtual void
    init (cli::scanner&) = 0;

    // Can be overriden by custom request dispatcher to initialize
    // sub-modules.
    //
    virtual void
    init (const name_values&);

    virtual void
    init (const name_values&, log&);

    virtual bool
    handle (request&, response&) = 0;

    virtual bool
    handle (request&, response&, log&);

    // web::module interface.
    //
  public:
    // Custom request dispatcher can aggregate its own option descriptions
    // with sub-modules option descriptions. In this case it should still call
    // the base implementation in order to include the brep::module's options.
    //
    virtual option_descriptions
    options ();

  private:
    virtual void
    version (log&);

    // Can be overriden by the module implementation to log version, etc.
    //
    virtual void
    version () {}

    name_values
    expand_options (const name_values&);

    // Diagnostics implementation details.
    //
  protected:
    log* log_ {nullptr}; // Diagnostics backend provided by the web server.

  private:
    // Extract function name from a __PRETTY_FUNCTION__.
    // Throw invalid_argument if fail to parse.
    //
    static string
    func_name (const char* pretty_name);

    void
    log_write (const diag_data&) const;

  protected:
    const diag_epilogue log_writer_;
  };
}

#endif // MOD_MODULE_HXX