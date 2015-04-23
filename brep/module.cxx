// file      : brep/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <brep/module>

#include <stdexcept>
#include <string>
#include <functional> // bind()
#include <cstring>    // strncmp()

#include <httpd/httpd.h>

#include <web/module>

#include <web/apache/log>

using namespace std;
using namespace placeholders; // For std::bind's _1, etc.

namespace brep
{
  void module::
  handle (request& rq, response& rs, log& l)
  {
    log_ = &l;
    const basic_mark error (severity::error, log_writer_, __PRETTY_FUNCTION__);

    try
    {
      handle (rq, rs);
    }
    catch (const invalid_request& e)
    {
      if (e.description.empty ())
      {
        rs.status (e.status);
      }
      else
      {
        try
        {
          rs.content (e.status, "text/html;charset=utf-8") << e.description;
        }
        catch (const sequence_error& se)
        {
          error << se.what ();
          rs.status (e.status);
        }
      }
    }
    catch (server_error& e) // Non-const because of move() below.
    {
      log_write (move (e.data));
      rs.status (HTTP_INTERNAL_SERVER_ERROR);
    }
    catch (const exception& e)
    {
      error << e.what ();
      rs.status (HTTP_INTERNAL_SERVER_ERROR);
    }
    catch (...)
    {
      error << "unknown error";
      rs.status (HTTP_INTERNAL_SERVER_ERROR);
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
  func_name (const string& pretty_name)
  {
    string::size_type b (0);
    string::size_type e (pretty_name.find (' '));

    // Position b at beginning of supposed function name,
    //
    if (e != string::npos && !strncmp (pretty_name.c_str (), "virtual ", 8))
    {
      // Skip keyword virtual.
      //
      b = pretty_name.find_first_not_of (' ', e);
      e = pretty_name.find (' ', b);
    }

    if (pretty_name.find ('(', b) > e)
    {
      // Not a constructor nor destructor. Skip type or *.
      //
      b = pretty_name.find_first_not_of (' ', e);
    }

    if (b != string::npos)
    {
      // Position e at the last character of supposed function name.
      //
      e = pretty_name.find_last_of (')');

      if (e != string::npos && e > b)
      {
        size_t d (1);

        while (--e > b && d)
        {
          switch (pretty_name[e])
          {
          case ')': ++d; break;
          case '(': --d; break;
          }
        }

        if (!d)
        {
          return pretty_name[b] == '(' && pretty_name[e] == ')' ?
            // Not a name yet, go deeper.
            //
            func_name (string(pretty_name, b + 1, e - b - 1)) :
            // Got the name.
            //
            string (pretty_name, b, e - b + 1);
        }
      }
    }

    throw invalid_argument ("");
  }

  void module::
  log_write (diag_data&& d) const
  {
    if (log_ == nullptr)
      return; // No backend yet.

    auto al = dynamic_cast<::web::apache::log*> (log_);

    if (al)
    {
      // Considered using lambda for mapping but looks too verbose while can
      // be a bit safer in runtime.
      //
      static int s[] = { APLOG_ERR, APLOG_WARNING, APLOG_INFO, APLOG_TRACE1 };

      for (const auto& e : d)
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

        al->write (e.loc.file.c_str(),
                   e.loc.line,
                   name.c_str(),
                   s[static_cast<int> (e.sev)],
                   e.msg.c_str());
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
