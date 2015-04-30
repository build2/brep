// file      : brep/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <brep/module>

#include <httpd/httpd.h>
#include <httpd/http_log.h>

#include <memory> // unique_ptr
#include <string>
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
        std::ostream& o = rs.content (500, "text/plain;charset=utf-8");

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

    int argc = 0;
    std::unique_ptr<const char*[]> argv (new const char*[options.size () * 2]);

    for (const auto& nv: options)
    {
      argv[argc++] = nv.name.c_str ();
      argv[argc++] = nv.value.c_str ();
    }

    try
    {
      {
        // Read module implementation configuration.
        //
        cli::argv_file_scanner s (0,
                                  argc,
                                  const_cast<char**> (argv.get ()),
                                  "conf");

        init (s);
      }

      // Read brep::module configuration.
      //
      cli::argv_file_scanner s (0,
                                argc,
                                const_cast<char**> (argv.get ()),
                                "conf");

      module_options o (s,
                        ::cli::unknown_mode::skip,
                        ::cli::unknown_mode::skip);

      verb_ = o.verb ();
    }
    catch (const server_error& e)
    {
      log_write (e.data);
      throw runtime_error ("initialization failed");
    }
    catch (const cli::exception& e)
    {
      std::ostringstream o;
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
// virtual std::string (* (* brep::search::func(std::string (* (*)(char))(int)\
// ,std::string (* (*)(wchar_t))(int)) const)(int, int))(int)
//
  string module::
  func_name (const char* pretty_name)
  {
    const char* e = strchr (pretty_name, ')');

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

    auto al = dynamic_cast<::web::apache::log*> (log_);

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

    //@@ Cast log_ to apache::log and write the records.
    //

    //@@ __PRETTY_FUNCTION__ contains a lot of fluff that we probably
    // don't want in the logs (like return value and argument list;
    // though the argument list would distinguish between several
    // overloads). If that's the case, then this is probably the
    // best place to process the name and convert something like:
    //
    // void module::handle(request, response)
    //
    // To just:
    //
    // module::handle
    //
    // Note to someone who is going to implement this: searching for a
    // space to determine the end of the return type may not work if
    // the return type is, say, a template id or a pointer to function
    // type. It seems a more robust approach would be to scan backwards
    // until we find the first ')' -- this got to be the end of the
    // function argument list. Now we continue scanning backwards keeping
    // track of the ')' vs '(' balance (arguments can also be of pointer
    // to function type). Once we see an unbalanced '(', then we know this
    // is the beginning of the argument list. Everything between it and
    // the preceding space is the qualified function name. Good luck ;-).
    //
    // If we also use the name in handle() above (e.g., to return to
    // the user as part of 505), then we should do it there as well
    // (in which case factoring this functionality into a separate
    // function seem to make a lot of sense).
    //
  }
}
