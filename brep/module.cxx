// file      : brep/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/module>

#include <httpd.h>
#include <http_log.h>

#include <vector>
#include <string>
#include <ostream>
#include <sstream>
#include <cstring>    // strncmp()
#include <stdexcept>
#include <functional> // bind()

#include <web/module>
#include <web/apache/log>

#include <brep/options>

using namespace std;
using namespace placeholders; // For std::bind's _1, etc.

namespace brep
{
  using namespace cli;

  // module
  //
  void module::
  handle (request& rq, response& rs, log& l)
  {
    log_ = &l;

    try
    {
      handle (rq, rs);
    }
    catch (const server_error& e)
    {
      log_write (e.data);

      try
      {
        static const char* sev_str[] = {"error", "warning", "info", "trace"};
        ostream& o (rs.content (500, "text/plain;charset=utf-8"));

        for (const auto& d: e.data)
        {
          string name;

          try
          {
            name = func_name (d.name);
          }
          catch (const invalid_argument&)
          {
            // Log "pretty" function description, see in log file & fix.
            name = d.name;
          }

          o << name << ": " << sev_str[static_cast<size_t> (d.sev)] << ": "
            << d.msg << endl;
        }
      }
      catch (const sequence_error&)
      {
        // We tried to return the error status/description but some
        // content has already been written. Nothing we can do about
        // it.
      }
    }
  }

  // Parse options with a cli-generated scanner. Options verb and conf are
  // recognized by brep::module::init while others to be interpreted by the
  // derived class init method. If there is an option which can not be
  // interpreted not by brep::module::init nor by derived class init method
  // then web server is terminated with a corresponding error message being
  // logged.
  //
  void module::
  init (const name_values& options, log& log)
  {
    log_ = &log;
    vector<const char*> argv;

    for (const auto& nv: options)
    {
      argv.push_back (nv.name.c_str ());

      if (nv.value)
        argv.push_back (nv.value->c_str ());
    }

    int argc (argv.size ());

    try
    {
      {
        // Read module implementation configuration.
        //
        argv_file_scanner s (0,
                             argc,
                             const_cast<char**> (argv.data ()),
                             "conf");

        init (s);
      }

      // Read brep::module configuration.
      //
      argv_file_scanner s (0,
                           argc,
                           const_cast<char**> (argv.data ()),
                           "conf");

      options::module o (s, unknown_mode::skip, unknown_mode::skip);
      verb_ = o.verb ();
    }
    catch (const server_error& e)
    {
      log_write (e.data);
      throw runtime_error ("initialization failed");
    }
    catch (const cli::exception& e)
    {
      ostringstream o;
      e.print (o);
      throw runtime_error (o.str ());
    }
  }

  module::
  module (): log_writer_ (bind (&module::log_write, this, _1)) {}

  // Custom copy constructor is required to initialize log_writer_ properly.
  //
  module::
  module (const module& m): module ()  {verb_ = m.verb_;}

// For function func declared like this:
// using B = std::string (*)(int);
// using A = B (*)(int,int);
// A func(B (*)(char),B (*)(wchar_t));
// __PRETTY_FUNCTION__ looks like this:
// virtual std::string (* (* brep::search::func(std::string (* (*)(char))(int)
// ,std::string (* (*)(wchar_t))(int)) const)(int, int))(int)
//
  string module::
  func_name (const char* pretty_name)
  {
    const char* e (strchr (pretty_name, ')'));

    if (e && e > pretty_name)
    {
      // Position e at last matching '(' which is the beginning of the
      // argument list..
      //
      size_t d (1);

      do
      {
        switch (*--e)
        {
        case ')': ++d; break;
        case '(': --d; break;
        }
      }
      while (d && e > pretty_name);

      if (!d && e > pretty_name)
      {
        // Position e at the character following the function name.
        //
        while (e > pretty_name &&
               (*e != '(' || *(e - 1) == ' ' || *(e - 1) == ')'))
          --e;

        if (e > pretty_name)
        {
          // Position b at the beginning of the qualified function name.
          //
          const char* b (e);
          while (--b > pretty_name && *b != ' ');
          if (*b == ' ') ++b;

          return string (b, e - b);
        }
      }
    }

    throw invalid_argument ("::brep::module::func_name");
  }

  void module::
  log_write (const diag_data& d) const
  {
    if (log_ == nullptr)
      return; // No backend yet.

    //@@ Cast log_ to apache::log and write the records.
    //
    auto al (dynamic_cast<::web::apache::log*> (log_));

    if (al)
    {
      // Considered using lambda for mapping but looks too verbose while can
      // be a bit safer in runtime.
      //
      static int s[] = {APLOG_ERR, APLOG_WARNING, APLOG_INFO, APLOG_TRACE1};

      for (const auto& e: d)
      {
        string name;

        try
        {
          name = func_name (e.name);
        }
        catch (const invalid_argument&)
        {
          // Log "pretty" function description, see in log file & fix.
          name = e.name;
        }

        al->write (e.loc.file.c_str (),
                   e.loc.line,
                   name.c_str (),
                   s[static_cast<size_t> (e.sev)],
                   e.msg.c_str ());
      }
    }
  }

  // module::param_scanner
  //
  module::param_scanner::
  param_scanner (const name_values& nv) noexcept
      : name_values_ (nv),
        i_ (nv.begin ()),
        name_ (true)
  {
  }

  bool module::param_scanner::
  more ()
  {
    return i_ != name_values_.end ();
  }

  const char* module::param_scanner::
  peek ()
  {
    if (i_ != name_values_.end ())
      return name_ ? i_->name.c_str () : i_->value->c_str ();
    else
      throw eos_reached ();
  }

  const char* module::param_scanner::
  next ()
  {
    if (i_ != name_values_.end ())
    {
      const char* r (name_ ? i_->name.c_str () : i_->value->c_str ());
      skip ();
      return r;
    }
    else
      throw eos_reached ();
  }

  void module::param_scanner::
  skip ()
  {
    if (i_ != name_values_.end ())
    {
      if (name_)
      {
        if (i_->value)
          name_ = false;
        else
          ++i_;
      }
      else
      {
        ++i_;
        name_ = true;
      }
    }
    else
      throw eos_reached ();
  }
}
