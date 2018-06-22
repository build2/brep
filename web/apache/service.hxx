// file      : web/apache/service.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef WEB_APACHE_SERVICE_HXX
#define WEB_APACHE_SERVICE_HXX

#include <apr_pools.h>   // apr_pool_t
#include <apr_hooks.h>   // APR_HOOK_*

#include <httpd.h>       // request_rec, server_rec, HTTP_*, DECLINED
#include <http_config.h> // module, cmd_parms, ap_hook_*()

#include <map>
#include <memory>  // unique_ptr
#include <string>
#include <cassert>

#include <web/module.hxx>
#include <web/apache/log.hxx>
#include <web/apache/request.hxx>

namespace web
{
  namespace apache
  {
    // Apache has 3 configuration scopes: main server, virtual server, and
    // directory (location). It provides configuration scope-aware modules
    // with the ability to build a hierarchy of configuration contexts. Later,
    // when processing a request, Apache passes the appropriate directory
    // configuration context to the request handler.
    //
    // This Apache service implementation first makes a copy of the provided
    // (in the constructor below) module exemplar for each directory context.
    // It then initializes each of these "context exemplars" with the (merged)
    // set of configuration options. Finally, when handling a request, it
    // copies the corresponding "context exemplar" to create the "handling
    // instance". Note that the "context exemplars" are created as a copy of
    // the provided exemplar, which is never initialized. As a result, it is
    // possible to detect if the module's copy constructor is used to create a
    // "context exemplar" or a "handling instance".
    //
    class service: ::module
    {
    public:
      // Note that the module exemplar is stored by-reference.
      //
      template <typename M>
      service (const std::string& name, M& exemplar)
          : ::module
            {
              STANDARD20_MODULE_STUFF,
              nullptr,
              nullptr,
              nullptr,
              nullptr,
              nullptr,
              &register_hooks<M>,
              AP_MODULE_FLAG_NONE
            },
            name_ (name),
            exemplar_ (exemplar)
      {
        init_directives ();

        // Set configuration context management hooks.
        //
        // The overall process of building the configuration hierarchy for a
        // module is as follows:
        //
        // 1. Apache creates directory and server configuration contexts for
        //    scopes containing module-defined directives by calling the
        //    create_{server,dir}_context() callback functions. For directives
        //    at the server scope the special directory context is created as
        //    well.
        //
        // 2. Apache calls parse_option() function for each module-defined
        //    directive. The function parses the directives and places the
        //    resulting options into the corresponding configuration context.
        //    It also establishes the directory-server contexts relations.
        //
        // 3. Apache calls merge_server_context() function for each virtual
        //    server. The function complements virtual server context options
        //    with the ones from the main server.
        //
        // 4. Apache calls config_finalizer() which complements the directory
        //    contexts options with the ones from the enclosing servers.
        //
        // 5. Apache calls worker_initializer() which creates module exemplar
        //    for each directory configuration context that have
        //    'SetHandler <mod_name>' directive in effect for it.
        //
        // References:
        //   http://www.apachetutor.org/dev/config
        //   http://httpd.apache.org/docs/2.4/developer/modguide.html
        //   http://wiki.apache.org/httpd/ModuleLife
        //
        create_server_config = &create_server_context;
        create_dir_config = &create_dir_context;
        merge_server_config = &merge_server_context<M>;

        // instance<M> () is invented to delegate processing from apache
        // request handler C function to the service non static member
        // function. This appoach resticts number of service objects per
        // specific module implementation class with just one instance.
        //
        service*& srv (instance<M> ());
        assert (srv == nullptr);
        srv = this;
      }

      ~service ()
      {
        delete [] cmds;
      }

    private:
      template <typename M>
      static service*&
      instance () noexcept
      {
        static service* instance;
        return instance;
      }

      template <typename M>
      static void
      register_hooks (apr_pool_t*) noexcept
      {
        // The config_finalizer() function is called at the end of Apache
        // server configuration parsing.
        //
        ap_hook_post_config (&config_finalizer<M>, NULL, NULL, APR_HOOK_LAST);

        // The worker_initializer() function is called right after Apache
        // worker process is started. Called for every new process spawned.
        //
        ap_hook_child_init (
          &worker_initializer<M>, NULL, NULL, APR_HOOK_LAST);

        // The request_handler () function is called for each client request.
        //
        ap_hook_handler (&request_handler<M>, NULL, NULL, APR_HOOK_LAST);
      }

      template <typename M>
      static int
      config_finalizer (apr_pool_t*, apr_pool_t*, apr_pool_t*, server_rec* s)
        noexcept
      {
        instance<M> ()->finalize_config (s);
        return OK;
      }

      template <typename M>
      static void
      worker_initializer (apr_pool_t*, server_rec* s) noexcept
      {
        auto srv (instance<M> ());
        log l (s, srv);
        srv->template init_worker<M> (l);
      }

      template <typename M>
      static int
      request_handler (request_rec* r) noexcept;

    private:

      // Reflects the allowability of the request handling in the specific
      // configuration scope.
      //
      enum class request_handling
      {
        // Configuration scope has 'SetHandler <mod_name>' directive
        // specified. The module is allowed to handle a request in the scope.
        //
        allowed,

        // Configuration scope has 'SetHandler <other_mod_name>|None'
        // directive specified. The module is disallowed to handle a request
        // in the scope.
        //
        disallowed,

        //
        // Note that if there are several SetHandler directives specified
        // in the specific scope, then the latest one takes the precedence.

        // Configuration scope has no SetHandler directive specified. The
        // request handling allowability is established by the enclosing
        // scopes.
        //
        inherit
      };

      // Our representation of the Apache configuration context.
      //
      // The lifetime of this object is under the control of the Apache API,
      // which treats it as a raw sequence of bytes. In order not to tinker
      // with the C-style structures and APR memory pools, we will keep it a
      // (C++11) POD type with just the members required to maintain the
      // context hierarchy.
      //
      // We will then use the pointers to these context objects as keys in
      // maps to (1) the corresponding application-level option lists during
      // the configuration cycle and to (2) the corresponding module exemplar
      // during the HTTP request handling phase. We will also use the same
      // type for both directory and server configuration contexts.
      //
      struct context
      {
        // Outer (server) configuration context for the directory
        // configuration context, NULL otherwise.
        //
        context* server = nullptr;

        // If module directives appear directly in the server configuration
        // scope, Apache creates a special directory context for them. This
        // context appears at the same hierarchy level as the user-defined
        // directory contexts of the same server scope.
        //
        bool special;

        // Request handling allowability for the corresponding configuration
        // scope.
        //
        request_handling handling = request_handling::inherit;

        // Create the server configuration context.
        //
        context (): special (false) {}

        // Create the directory configuration context. Due to the Apache API
        // implementation details it is not possible to detect the enclosing
        // server configuration context at the time of directory context
        // creation. As a result, the server member is set by the module's
        // parse_option() function.
        //
        context (bool s): special (s) {}

        // Ensure the object is only destroyed by Apache.
        //
        ~context () = delete;
      };

      static context*
      context_cast (void* config) noexcept
      {return static_cast<context*> (config);}

    private:
      void
      init_directives ();

      // Create the server configuration context. Called by the Apache API
      // whenever a new object of that type is required.
      //
      static void*
      create_server_context (apr_pool_t*, server_rec*) noexcept;

      // Create the server directory configuration context. Called by the
      // Apache API whenever a new object of that type is required.
      //
      static void*
      create_dir_context (apr_pool_t*, char* dir) noexcept;

      template <typename M>
      static void*
      merge_server_context (apr_pool_t*, void* enclosing, void* enclosed)
        noexcept
      {
        instance<M> ()->complement (
          context_cast (enclosed), context_cast (enclosing));

        return enclosed;
      }

      static const char*
      parse_option (cmd_parms* parms, void* conf, const char* args) noexcept;

      const char*
      add_option (context*, const char* name, optional<std::string> value);

      void
      finalize_config (server_rec*);

      void
      clear_config ();

      // Complement the enclosed context with options of the enclosing one.
      // If the 'handling' member of the enclosed context is set to
      // request_handling::inherit value, assign it a value from the enclosing
      // context.
      //
      void
      complement (context* enclosed, context* enclosing);

      template <typename M>
      void
      init_worker (log&);

      template <typename M>
      int
      handle (request&, const context*, log&) const;

    private:
      std::string name_;
      module& exemplar_;
      option_descriptions option_descriptions_;

      // The context objects pointed to by the key can change during the
      // configuration phase.
      //
      using options = std::map<context*, name_values>;
      options options_;

      // The context objects pointed to by the key can not change during the
      // request handling phase.
      //
      using exemplars = std::map<const context*, std::unique_ptr<module>>;
      exemplars exemplars_;

      bool options_parsed_ = false;
      bool version_logged_ = false;
    };
  }
}

#include <web/apache/service.txx>

#endif // WEB_APACHE_SERVICE_HXX
