// file      : web/apache/service.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <web/apache/service.hxx>

#include <apr_pools.h> // apr_palloc()

#include <httpd.h>       // server_rec
#include <http_config.h> // command_rec, cmd_*, ap_get_module_config()

#include <memory>    // unique_ptr
#include <string>
#include <cassert>
#include <utility>   // move()
#include <cstring>   // strlen(), strcmp()
#include <exception>

#include <libbutl/optional.hxx>

#include <web/module.hxx>
#include <web/apache/log.hxx>

using namespace std;

namespace web
{
  namespace apache
  {
    void service::
    init_directives ()
    {
      assert (cmds == nullptr);

      // Fill apache module directive definitions. Directives share common
      // name space in apache configuration file, so to prevent name clash
      // have to form directive name as a combination of module and option
      // names: <module name>-<option name>. This why for option bar of module
      // foo the corresponding directive will appear in apache configuration
      // file as foo-bar.
      //
      const option_descriptions& od (exemplar_.options ());
      unique_ptr<command_rec[]> directives (new command_rec[od.size () + 2]);
      command_rec* d (directives.get ());

      for (const auto& o: od)
      {
        auto i (
          option_descriptions_.emplace (name_ + "-" + o.first, o.second));
        assert (i.second);

        *d++ =
          {
            i.first->first.c_str (),
            reinterpret_cast<cmd_func> (parse_option),
            this,

            // Allow directives in both server and directory configuration
            // scopes.
            //
            RSRC_CONF | ACCESS_CONF,

            // Move away from TAKE1 to be able to handle empty string and
            // no-value.
            //
            RAW_ARGS,

            nullptr
          };
      }

      // Track if the module is allowed to handle a request in the specific
      // configuration scope. The module exemplar will be created (and
      // initialized) only for configuration contexts that have
      // 'SetHandler <mod_name>' in effect for the corresponding scope.
      //
      *d++ =
        {
          "SetHandler",
          reinterpret_cast<cmd_func> (parse_option),
          this,
          RSRC_CONF | ACCESS_CONF,
          RAW_ARGS,
          nullptr
        };

      *d = {nullptr, nullptr, nullptr, 0, RAW_ARGS, nullptr};
      cmds = directives.release ();
    }

    void* service::
    create_server_context (apr_pool_t* pool, server_rec*) noexcept
    {
      // Create the object using the configuration memory pool provided by the
      // Apache API. The lifetime of the object is equal to the lifetime of
      // the pool.
      //
      void* p (apr_palloc (pool, sizeof (context)));
      assert (p != nullptr);
      return new (p) context ();
    }

    void* service::
    create_dir_context (apr_pool_t* pool, char* dir) noexcept
    {
      // Create the object using the configuration memory pool provided by the
      // Apache API. The lifetime of the object is equal to the lifetime of
      // the pool.
      //
      void* p (apr_palloc (pool, sizeof (context)));
      assert (p != nullptr);

      // For the user-defined directory configuration context dir is the path
      // of the corresponding directive. For the special server directory
      // invented by Apache for server scope directives, dir is NULL.
      //
      return new (p) context (dir == nullptr);
    }

    const char* service::
    parse_option (cmd_parms* parms, void* conf, const char* args) noexcept
    {
      service& srv (*reinterpret_cast<service*> (parms->cmd->cmd_data));

      if (srv.options_parsed_)
        // Apache have started the second pass of its messy initialization
        // cycle (more details at http://wiki.apache.org/httpd/ModuleLife).
        // This time we are parsing for real. Cleanup the existing config, and
        // start building the new one.
        //
        srv.clear_config ();

      // 'args' is an optionally double-quoted string. It uses double quotes
      // to distinguish empty string from no-value case.
      //
      assert (args != nullptr);

      optional<string> value;
      if (auto l = strlen (args))
        value = l >= 2 && args[0] == '"' && args[l - 1] == '"'
          ? string (args + 1, l - 2)
          : args;

      // Determine the directory and server configuration contexts for the
      // option.
      //
      context* dir_context (context_cast (conf));
      assert (dir_context != nullptr);

      server_rec* server (parms->server);
      assert (server != nullptr);
      assert (server->module_config != nullptr);

      context* srv_context (
        context_cast (ap_get_module_config (server->module_config, &srv)));

      assert (srv_context != nullptr);

      // Associate the directory configuration context with the enclosing
      // server configuration context.
      //
      context*& s (dir_context->server);
      if (s == nullptr)
        s = srv_context;
      else
        assert (s == srv_context);

      // If the option appears in the special directory configuration context,
      // add it to the enclosing server context instead. This way it will be
      // possible to complement all server-enclosed contexts (including this
      // special one) with the server scope options.
      //
      context* c (dir_context->special ? srv_context : dir_context);

      if (dir_context->special)
        //
        // Make sure the special directory context is also in the option lists
        // map. Later the context will be populated with an enclosing server
        // context options.
        //
        srv.options_.emplace (dir_context, name_values ());

      const char* name (parms->cmd->name);
      if (strcmp (name, "SetHandler") == 0)
      {
        // Keep track of a request handling allowability.
        //
        srv.options_.emplace (c, name_values ()).first->first->handling =
          value && *value == srv.name_
          ? request_handling::allowed
          : request_handling::disallowed;

        return 0;
      }

      return srv.add_option (c, name, move (value));
    }

    const char* service::
    add_option (context* ctx, const char* name, optional<string> value)
    {
      auto i (option_descriptions_.find (name));
      assert (i != option_descriptions_.end ());

      // Check that option value presense is expected.
      //
      if (i->second != static_cast<bool> (value))
        return value ? "unexpected value" : "value expected";

      options_[ctx].emplace_back (name + name_.length () + 1, move (value));
      return 0;
    }

    void service::
    complement (context* enclosed, context* enclosing)
    {
      auto i (options_.find (enclosing));

      // The enclosing context may have no options. It can be the context of a
      // server that has no configuration directives in it's immediate scope,
      // but has ones in it's enclosed scope (directory or virtual server).
      //
      if (i != options_.end ())
      {
        const name_values& src (i->second);
        name_values& dest (options_[enclosed]);
        dest.insert (dest.begin (), src.begin (), src.end ());
      }

      if (enclosed->handling == request_handling::inherit)
        enclosed->handling = enclosing->handling;
    }

    void service::
    finalize_config (server_rec* s)
    {
      if (!version_logged_)
      {
        log l (s, this);
        exemplar_.version (l);
        version_logged_ = true;
      }

      // Complement directory configuration contexts with options of the
      // enclosing server configuration context. By this time virtual server
      // contexts are already complemented with the main server configuration
      // context options as a result of the merge_server_context() calls.
      //
      for (const auto& o: options_)
      {
        // Is a directory configuration context.
        //
        if (o.first->server != nullptr)
          complement (o.first, o.first->server);
      }

      options_parsed_ = true;
    }

    void service::
    clear_config ()
    {
      options_.clear ();
      options_parsed_ = false;
    }
  }
}
