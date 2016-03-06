// file      : web/apache/service.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <unistd.h> // getppid()
#include <signal.h> // kill()

#include <http_log.h>

#include <utility>   // move()
#include <exception>

namespace web
{
  namespace apache
  {
    template <typename M>
    void service::
    init_worker (log& l)
    {
      const std::string func_name (
        "web::apache::service<" + name_ + ">::init_worker");

      try
      {
        const M* exemplar (dynamic_cast<const M*> (&exemplar_));
        assert (exemplar != nullptr);

        // For each directory configuration context create the module exemplar
        // as a deep copy of the exemplar_ member and initialize it with the
        // context-specific option list. Note that there can be contexts
        // having no module options specified for them and no options
        // inherited from enclosing contexts. Such contexts will not appear in
        // the options_ map. Meanwhile 'SetHandler <modname>' directive can be
        // in effect for such contexts, and we should be ready to handle
        // requests for them (by using the "root exemplar").
        //
        for (const auto& o: options_)
        {
          const context* c (o.first);

          if (c->server != nullptr) // Is a directory configuration context.
          {
            auto r (
              exemplars_.emplace (
                make_context_id (c),
                std::unique_ptr<module> (new M (*exemplar))));

            r.first->second->init (o.second, l);
          }
        }

        // Options are not needed anymore. Free up the space.
        //
        options_.clear ();

        // Initialize the "root exemplar" by default (with no options). It
        // will be used to handle requests for configuration contexts having
        // no options specified, and no options inherited from enclosing
        // contexts.
        //
        exemplar_.init (name_values (), l);
      }
      catch (const std::exception& e)
      {
        l.write (nullptr, 0, func_name.c_str (), APLOG_EMERG, e.what ());

        // Terminate the root apache process. Indeed we can only try to
        // terminate the process, and most likely will fail in a production
        // environment, where the apache root process usually runs under root,
        // and worker processes run under some other user. This is why the
        // implementation should consider the possibility of not being
        // initialized at the time of HTTP request processing. In such a case
        // it should respond with an internal server error (500 HTTP status),
        // reporting misconfiguration.
        //
        kill (getppid (), SIGTERM);
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
        kill (getppid (), SIGTERM);
      }
    }

    template <typename M>
    int service::
    request_handler (request_rec* r) noexcept
    {
      auto srv (instance<M> ());
      if (!r->handler || srv->name_ != r->handler) return DECLINED;

      assert (r->per_dir_config != nullptr);

      // Obtain the request-associated configuration context id.
      //
      context_id id (
        make_context_id (ap_get_module_config (r->per_dir_config, srv)));

      assert (!is_null (id));

      request rq (r);
      log lg (r->server, srv);
      return srv->template handle<M> (rq, id, lg);
    }

    template <typename M>
    int service::
    handle (request& rq, context_id id, log& lg) const
    {
      static const std::string func_name (
        "web::apache::service<" + name_ + ">::handle");

      try
      {
        auto i (exemplars_.find (id));

        // Use the context-specific exemplar if found, otherwise use the
        // default one.
        //
        const module* exemplar (i != exemplars_.end ()
                                ? i->second.get ()
                                : &exemplar_);

        const M* e (dynamic_cast<const M*> (exemplar));
        assert (e != nullptr);

        M m (*e);

        if (static_cast<module&> (m).handle (rq, rq, lg))
          return rq.flush ();

        if (!rq.get_write_state ())
          return DECLINED;

        lg.write (nullptr, 0, func_name.c_str (), APLOG_ERR,
                  "handling declined while unbuffered content "
                  "has been written");
      }
      catch (const invalid_request& e)
      {
        if (!e.content.empty () && !rq.get_write_state ())
        {
          try
          {
            rq.content (e.status, e.type) << e.content;
            return rq.flush ();
          }
          catch (const std::exception& e)
          {
            lg.write (nullptr, 0, func_name.c_str (), APLOG_ERR, e.what ());
          }
        }

        return e.status;
      }
      catch (const std::exception& e)
      {
        lg.write (nullptr, 0, func_name.c_str (), APLOG_ERR, e.what ());

        if (*e.what () && !rq.get_write_state ())
        {
          try
          {
            rq.content (HTTP_INTERNAL_SERVER_ERROR, "text/plain;charset=utf-8")
              << e.what ();

            return rq.flush ();
          }
          catch (const std::exception& e)
          {
            lg.write (nullptr, 0, func_name.c_str (), APLOG_ERR, e.what ());
          }
        }
      }
      catch (...)
      {
        lg.write (nullptr, 0, func_name.c_str (), APLOG_ERR, "unknown error");

        if (!rq.get_write_state ())
        {
          try
          {
            rq.content (HTTP_INTERNAL_SERVER_ERROR, "text/plain;charset=utf-8")
              << "unknown error";

            return rq.flush ();
          }
          catch (const std::exception& e)
          {
            lg.write (nullptr, 0, func_name.c_str (), APLOG_ERR, e.what ());
          }
        }
      }

      return HTTP_INTERNAL_SERVER_ERROR;
    }
  }
}
