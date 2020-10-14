// -*- C++ -*-
//
// This file was generated by CLI, a command line interface
// compiler for C++.
//

#ifndef LOAD_LOAD_OPTIONS_HXX
#define LOAD_LOAD_OPTIONS_HXX

// Begin prologue.
//
//
// End prologue.

#include <iosfwd>
#include <string>
#include <cstddef>
#include <exception>

#ifndef CLI_POTENTIALLY_UNUSED
#  if defined(_MSC_VER) || defined(__xlC__)
#    define CLI_POTENTIALLY_UNUSED(x) (void*)&x
#  else
#    define CLI_POTENTIALLY_UNUSED(x) (void)x
#  endif
#endif

namespace cli
{
  class usage_para
  {
    public:
    enum value
    {
      none,
      text,
      option
    };

    usage_para (value);

    operator value () const 
    {
      return v_;
    }

    private:
    value v_;
  };

  class unknown_mode
  {
    public:
    enum value
    {
      skip,
      stop,
      fail
    };

    unknown_mode (value);

    operator value () const 
    {
      return v_;
    }

    private:
    value v_;
  };

  // Exceptions.
  //

  class exception: public std::exception
  {
    public:
    virtual void
    print (::std::ostream&) const = 0;
  };

  ::std::ostream&
  operator<< (::std::ostream&, const exception&);

  class unknown_option: public exception
  {
    public:
    virtual
    ~unknown_option () throw ();

    unknown_option (const std::string& option);

    const std::string&
    option () const;

    virtual void
    print (::std::ostream&) const;

    virtual const char*
    what () const throw ();

    private:
    std::string option_;
  };

  class unknown_argument: public exception
  {
    public:
    virtual
    ~unknown_argument () throw ();

    unknown_argument (const std::string& argument);

    const std::string&
    argument () const;

    virtual void
    print (::std::ostream&) const;

    virtual const char*
    what () const throw ();

    private:
    std::string argument_;
  };

  class missing_value: public exception
  {
    public:
    virtual
    ~missing_value () throw ();

    missing_value (const std::string& option);

    const std::string&
    option () const;

    virtual void
    print (::std::ostream&) const;

    virtual const char*
    what () const throw ();

    private:
    std::string option_;
  };

  class invalid_value: public exception
  {
    public:
    virtual
    ~invalid_value () throw ();

    invalid_value (const std::string& option,
                   const std::string& value,
                   const std::string& message = std::string ());

    const std::string&
    option () const;

    const std::string&
    value () const;

    const std::string&
    message () const;

    virtual void
    print (::std::ostream&) const;

    virtual const char*
    what () const throw ();

    private:
    std::string option_;
    std::string value_;
    std::string message_;
  };

  class eos_reached: public exception
  {
    public:
    virtual void
    print (::std::ostream&) const;

    virtual const char*
    what () const throw ();
  };

  // Command line argument scanner interface.
  //
  // The values returned by next() are guaranteed to be valid
  // for the two previous arguments up until a call to a third
  // peek() or next().
  //
  class scanner
  {
    public:
    virtual
    ~scanner ();

    virtual bool
    more () = 0;

    virtual const char*
    peek () = 0;

    virtual const char*
    next () = 0;

    virtual void
    skip () = 0;
  };

  class argv_scanner: public scanner
  {
    public:
    argv_scanner (int& argc, char** argv, bool erase = false);
    argv_scanner (int start, int& argc, char** argv, bool erase = false);

    int
    end () const;

    virtual bool
    more ();

    virtual const char*
    peek ();

    virtual const char*
    next ();

    virtual void
    skip ();

    private:
    int i_;
    int& argc_;
    char** argv_;
    bool erase_;
  };

  template <typename X>
  struct parser;
}

#include <vector>

#include <string>

#include <cstdint>

#include <libbrep/types.hxx>

class options
{
  public:
  options ();

  options (int& argc,
           char** argv,
           bool erase = false,
           ::cli::unknown_mode option = ::cli::unknown_mode::fail,
           ::cli::unknown_mode argument = ::cli::unknown_mode::stop);

  options (int start,
           int& argc,
           char** argv,
           bool erase = false,
           ::cli::unknown_mode option = ::cli::unknown_mode::fail,
           ::cli::unknown_mode argument = ::cli::unknown_mode::stop);

  options (int& argc,
           char** argv,
           int& end,
           bool erase = false,
           ::cli::unknown_mode option = ::cli::unknown_mode::fail,
           ::cli::unknown_mode argument = ::cli::unknown_mode::stop);

  options (int start,
           int& argc,
           char** argv,
           int& end,
           bool erase = false,
           ::cli::unknown_mode option = ::cli::unknown_mode::fail,
           ::cli::unknown_mode argument = ::cli::unknown_mode::stop);

  options (::cli::scanner&,
           ::cli::unknown_mode option = ::cli::unknown_mode::fail,
           ::cli::unknown_mode argument = ::cli::unknown_mode::stop);

  // Option accessors.
  //
  const bool&
  ignore_unknown () const;

  const bool&
  force () const;

  const bool&
  shallow () const;

  const std::string&
  tenant () const;

  bool
  tenant_specified () const;

  const brep::path&
  overrides_file () const;

  bool
  overrides_file_specified () const;

  const std::string&
  db_user () const;

  bool
  db_user_specified () const;

  const std::string&
  db_password () const;

  bool
  db_password_specified () const;

  const std::string&
  db_name () const;

  bool
  db_name_specified () const;

  const std::string&
  db_host () const;

  bool
  db_host_specified () const;

  const std::uint16_t&
  db_port () const;

  bool
  db_port_specified () const;

  const brep::path&
  bpkg () const;

  bool
  bpkg_specified () const;

  const brep::strings&
  bpkg_option () const;

  bool
  bpkg_option_specified () const;

  const std::string&
  pager () const;

  bool
  pager_specified () const;

  const std::vector<std::string>&
  pager_option () const;

  bool
  pager_option_specified () const;

  const bool&
  help () const;

  const bool&
  version () const;

  // Print usage information.
  //
  static ::cli::usage_para
  print_usage (::std::ostream&,
               ::cli::usage_para = ::cli::usage_para::none);

  // Implementation details.
  //
  protected:
  bool
  _parse (const char*, ::cli::scanner&);

  private:
  bool
  _parse (::cli::scanner&,
          ::cli::unknown_mode option,
          ::cli::unknown_mode argument);

  public:
  bool ignore_unknown_;
  bool force_;
  bool shallow_;
  std::string tenant_;
  bool tenant_specified_;
  brep::path overrides_file_;
  bool overrides_file_specified_;
  std::string db_user_;
  bool db_user_specified_;
  std::string db_password_;
  bool db_password_specified_;
  std::string db_name_;
  bool db_name_specified_;
  std::string db_host_;
  bool db_host_specified_;
  std::uint16_t db_port_;
  bool db_port_specified_;
  brep::path bpkg_;
  bool bpkg_specified_;
  brep::strings bpkg_option_;
  bool bpkg_option_specified_;
  std::string pager_;
  bool pager_specified_;
  std::vector<std::string> pager_option_;
  bool pager_option_specified_;
  bool help_;
  bool version_;
};

// Print page usage information.
//
::cli::usage_para
print_usage (::std::ostream&,
             ::cli::usage_para = ::cli::usage_para::none);

#include <load/load-options.ixx>

// Begin epilogue.
//
//
// End epilogue.

#endif // LOAD_LOAD_OPTIONS_HXX
