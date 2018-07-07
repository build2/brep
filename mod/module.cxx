// file      : mod/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/module.hxx>

#include <httpd.h>
#include <http_log.h>

#include <sstream>
#include <cstring>    // strchr()
#include <functional> // bind()

#include <web/module.hxx>
#include <web/apache/log.hxx>

#include <mod/options.hxx>

using namespace std;
using namespace placeholders; // For std::bind's _1, etc.

namespace brep
{
  // handler
  //
  bool handler::
  handle (request& rq, response& rs, log& l)
  {
    log_ = &l;

    try
    {
      // Web server should terminate if initialization failed.
      //
      assert (initialized_);

      return handle (rq, rs);
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

    return true;
  }

  option_descriptions handler::
  convert (const cli::options& o)
  {
    option_descriptions r;
    append (r, o);
    return r;
  }

  void handler::
  append (option_descriptions& dst, const cli::options& src)
  {
    for (const auto& o: src)
    {
      bool v (!o.flag ());
      auto i (dst.emplace (o.name (), v));
      assert (i.first->second == v); // Consistent option/flag.

      for (const auto& a: o.aliases ())
      {
        i = dst.emplace (a, v);
        assert (i.first->second == v);
      }
    }
  }

  void handler::
  append (option_descriptions& dst, const option_descriptions& src)
  {
    for (const auto& o: src)
    {
      auto i (dst.emplace (o));
      assert (i.first->second == o.second); // Consistent option/flag.
    }
  }

  name_values handler::
  filter (const name_values& v, const option_descriptions& d)
  {
    name_values r;
    for (const auto& nv: v)
    {
      if (d.find (nv.name) != d.end ())
        r.push_back (nv);
    }

    return r;
  }

  // Convert CLI option descriptions to the general interface of option
  // descriptions, extend with brep::handler own option descriptions.
  //
  option_descriptions handler::
  options ()
  {
    option_descriptions r ({{"conf", true}});
    append (r, options::handler::description ());
    append (r, cli_options ());
    return r;
  }

  // Expand option list parsing configuration files.
  //
  name_values handler::
  expand_options (const name_values& v)
  {
    using namespace cli;

    vector<const char*> argv;
    for (const auto& nv: v)
    {
      argv.push_back (nv.name.c_str ());

      if (nv.value)
        argv.push_back (nv.value->c_str ());
    }

    int argc (argv.size ());
    argv_file_scanner s (0, argc, const_cast<char**> (argv.data ()), "conf");

    name_values r;
    const option_descriptions& o (options ());

    while (s.more ())
    {
      string n (s.next ());
      auto i (o.find (n));

      if (i == o.end ())
        throw unknown_argument (n);

      optional<string> v;
      if (i->second)
        v = s.next ();

      r.emplace_back (move (n), move (v));
    }

    return r;
  }

  // Parse options with a cli-generated scanner. Options verb and conf are
  // recognized by brep::handler::init while others to be interpreted by the
  // derived init(). If there is an option which can not be interpreted
  // neither by brep::handler nor by the derived class, then the web server
  // is terminated with a corresponding error message being logged. Though
  // this should not happen if the options() function returned the correct
  // set of options.
  //
  void handler::
  init (const name_values& options, log& log)
  {
    assert (!initialized_);

    log_ = &log;

    try
    {
      name_values opts (expand_options (options));

      // Read handler implementation configuration.
      //
      init (opts);

      // Read brep::handler configuration.
      //
      static option_descriptions od (
        convert (options::handler::description ()));

      name_values mo (filter (opts, od));
      name_value_scanner s (mo);
      options::handler o (s, cli::unknown_mode::fail, cli::unknown_mode::fail);

      verb_ = o.verbosity ();
      initialized_ = true;
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

  void handler::
  init (const name_values& options)
  {
    name_value_scanner s (options);
    init (s);
    assert (!s.more ()); // Handler didn't handle its options.
  }

  handler::
  handler (): log_writer_ (bind (&handler::log_write, this, _1)) {}

  // Custom copy constructor is required to initialize log_writer_ properly.
  //
  handler::
  handler (const handler& m): handler ()
  {
    verb_ = m.verb_;
    initialized_ = m.initialized_;
  }

// For function func declared like this:
// using B = std::string (*)(int);
// using A = B (*)(int,int);
// A func(B (*)(char),B (*)(wchar_t));
// __PRETTY_FUNCTION__ looks like this:
// virtual std::string (* (* brep::search::func(std::string (* (*)(char))(int)
// ,std::string (* (*)(wchar_t))(int)) const)(int, int))(int)
//
  string handler::
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

    throw invalid_argument ("::brep::handler::func_name");
  }

  void handler::
  log_write (const diag_data& d) const
  {
    if (log_ == nullptr)
      return; // No backend yet.

    //@@ Cast log_ to apache::log and write the records.
    //
    auto al (dynamic_cast<web::apache::log*> (log_));

    if (al)
    {
      // Considered using lambda for mapping but looks too verbose while can
      // be a bit safer in runtime.
      //
      // Use APLOG_INFO (as opposed to APLOG_TRACE1) as a mapping for
      // severity::trace. "LogLevel trace1" configuration directive switches
      // on the avalanche of log messages from various handlers. Would be good
      // to avoid wading through them.
      //
      static int s[] = {APLOG_ERR, APLOG_WARNING, APLOG_INFO, APLOG_INFO};

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

  void handler::
  version (log& l)
  {
    log_ = &l;
    version ();
  }

  // handler::name_value_scanner
  //
  handler::name_value_scanner::
  name_value_scanner (const name_values& nv) noexcept
      : name_values_ (nv),
        i_ (nv.begin ()),
        name_ (true)
  {
  }

  bool handler::name_value_scanner::
  more ()
  {
    return i_ != name_values_.end ();
  }

  const char* handler::name_value_scanner::
  peek ()
  {
    if (i_ != name_values_.end ())
      return name_ ? i_->name.c_str () : i_->value->c_str ();
    else
      throw cli::eos_reached ();
  }

  const char* handler::name_value_scanner::
  next ()
  {
    if (i_ != name_values_.end ())
    {
      const char* r (name_ ? i_->name.c_str () : i_->value->c_str ());
      skip ();
      return r;
    }
    else
      throw cli::eos_reached ();
  }

  void handler::name_value_scanner::
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
      throw cli::eos_reached ();
  }
}
