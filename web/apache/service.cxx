// file      : web/apache/service.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <web/apache/service>

#include <unistd.h> // getppid()
#include <signal.h> // kill()

#include <httpd.h>
#include <http_config.h>

#include <memory>    // unique_ptr
#include <string>
#include <cassert>
#include <utility>   // move()
#include <cstring>   // strlen()
#include <exception>

#include <web/module>

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
      const option_descriptions& od (exemplar_.options ());
      unique_ptr<command_rec[]> directives (new command_rec[od.size () + 1]);
      command_rec* d (directives.get ());

      for (const auto& o: od)
      {
        auto i (option_descriptions_.emplace (name_ + "-" + o.first, o.second));
        assert (i.second);

        *d++ =
          {
            i.first->first.c_str (),
            reinterpret_cast<cmd_func> (parse_option),
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
    parse_option (cmd_parms* parms, void*, const char* args) noexcept
    {
      // @@ Current implementation does not consider configuration context
      //    (server config, virtual host, directory) for directive parsing, nor
      //    for request handling.
      //
      service& srv (*reinterpret_cast<service*> (parms->cmd->cmd_data));

      if (srv.options_parsed_)
        // Apache is inside the second pass of its messy initialization cycle
        // (more details at http://wiki.apache.org/httpd/ModuleLife). Just
        // ignore it.
        //
        return 0;

      // 'args' is an optionally double-quoted string. It uses double quotes
      // to distinguish empty string from no-value case.
      //
      assert (args != nullptr);

      optional<string> value;
      if (auto l = strlen (args))
        value = l >= 2 && args[0] == '"' && args[l - 1] == '"'
          ? string (args + 1, l - 2)
          : args;

      return srv.add_option (parms->cmd->name, move (value));
    }

    const char* service::
    add_option (const char* name, optional<string> value)
    {
      auto i (option_descriptions_.find (name));
      assert (i != option_descriptions_.end ());

      // Check that option value presense is expected.
      //
      if (i->second != static_cast<bool> (value))
        return value ? "unexpected value" : "value expected";

      options_.emplace_back (name + name_.length () + 1, move (value));
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

        // Terminate the root apache process. Indeed we can only try to
        // terminate the process, and most likely will fail in a production
        // environment where the apache root process usually runs under root
        // and worker processes run under some other user. This is why the
        // implementation should consider the possibility of not being
        // initialized at the time of HTTP request processing. In such a case
        // it should respond with an internal server error (500 HTTP status),
        // reporting misconfiguration.
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
