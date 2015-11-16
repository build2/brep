// file      : web/apache/service.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <web/apache/service>

#include <unistd.h> // getppid()
#include <signal.h> // kill()

#include <httpd.h>
#include <http_config.h>

#include <memory>  // unique_ptr
#include <string>
#include <cassert>
#include <cstring> // strlen()
#include <exception>

using namespace std;

namespace web
{
  namespace apache
  {
    void service::
    init_directives ()
    {
      assert (cmds == nullptr);

      // Fill apache module directive definitions. Directives share
      // common name space in apache configuration file, so to prevent name
      // clash have to form directive name as a combination of module and
      // option names: <module name>-<option name>. This why for option
      // bar of module foo the corresponding directive will appear in apache
      // configuration file as foo-bar.
      //
      unique_ptr<command_rec[]> directives (
        new command_rec[option_names_.size () + 1]);

      command_rec* d (directives.get ());

      for (auto& o: option_names_)
      {
        o = name_ + "-" + o;

        *d++ =
          {
            o.c_str (),
            reinterpret_cast<cmd_func> (add_option),
            this,
            RSRC_CONF,
            // Move away from TAKE1 to be able to handle empty string and
            // no-value.
            //
            RAW_ARGS,
            nullptr
          };
      }

      *d = {nullptr, nullptr, nullptr, 0, RAW_ARGS, nullptr};

      cmds = directives.release ();
    }

    const char* service::
    add_option (cmd_parms* parms, void*, const char* args) noexcept
    {
      service& srv (*reinterpret_cast<service*> (parms->cmd->cmd_data));
      string name (parms->cmd->name + srv.name_.length () + 1);
      optional<string> value;

      // 'args' is an optionally double-quoted string. Use double quotes to
      // distinguish empty string from no-value case.
      //
      assert (args != nullptr);
      if (auto l = strlen (args))
        value = l >= 2 && args[0] == '"' && args[l - 1] == '"'
          ? string (args + 1, l - 2)
          : args;

      for (auto& v: srv.options_)
      {
        if (v.name == name)
        {
          v.value = value;
          return 0;
        }
      }

      srv.options_.emplace_back (name, value);
      return 0;
    }

    void service::
    init_worker (log& l) noexcept
    {
      const string func_name (
        "web::apache::service<" + name_ + ">::init_worker");

      try
      {
        exemplar_.init (options_, l);
      }
      catch (const exception& e)
      {
        l.write (nullptr, 0, func_name.c_str (), APLOG_EMERG, e.what ());

        // Terminate the root apache process.
        //
        ::kill (::getppid (), SIGTERM);
      }
      catch (...)
      {
        l.write (nullptr,
                 0,
                 func_name.c_str (),
                 APLOG_EMERG,
                 "unknown error");

        // Terminate the root apache process.
        //
        ::kill (::getppid (), SIGTERM);
      }
    }
  }
}
