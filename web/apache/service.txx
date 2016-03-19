// file      : web/apache/service.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <httpd.h>    // APEXIT_CHILDSICK
#include <http_log.h> // APLOG_*

#include <cstdlib>   // exit()
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

        // For each directory configuration context, for which the module is
        // allowed to handle a request, create the module exemplar as a deep
        // copy of the exemplar_ member, and initialize it with the
        // context-specific option list.
        //
        for (const auto& o: options_)
        {
          const context* c (o.first);

          if (c->server != nullptr && // Is a directory configuration context.
              c->handling == request_handling::allowed)
          {
            auto r (
              exemplars_.emplace (
                c,
                std::unique_ptr<module> (new M (*exemplar))));

            r.first->second->init (o.second, l);
          }
        }

        // Options are not needed anymore. Free up the space.
        //
        options_.clear ();
      }
      catch (const std::exception& e)
      {
        l.write (nullptr, 0, func_name.c_str (), APLOG_EMERG, e.what ());

        // Terminate the worker apache process. APEXIT_CHILDSICK indicates to
        // the root process that the worker have exited due to a resource
        // shortage. In this case the root process limits the rate of forking
        // until situation is resolved.
        //
        // If the root process fails to create any worker process on startup,
        // the behaviour depends on the Multi-Processing Module enabled. For
        // mpm_worker_module and mpm_event_module the root process terminates.
        // For mpm_prefork_module it keeps trying to create the worker process
        // at one-second intervals.
        //
        // If the root process loses all it's workers while running (for
        // example due to the MaxRequestsPerChild directive), and fails to
        // create any new ones, it keeps trying to create the worker process
        // at one-second intervals.
        //
        std::exit (APEXIT_CHILDSICK);
      }
      catch (...)
      {
        l.write (nullptr,
                 0,
                 func_name.c_str (),
                 APLOG_EMERG,
                 "unknown error");

        // Terminate the worker apache process.
        //
        std::exit (APEXIT_CHILDSICK);
      }
    }

    template <typename M>
    int service::
    request_handler (request_rec* r) noexcept
    {
      auto srv (instance<M> ());
      if (!r->handler || srv->name_ != r->handler) return DECLINED;

      assert (r->per_dir_config != nullptr);

      // Obtain the request-associated configuration context.
      //
      const context* cx (
        context_cast (ap_get_module_config (r->per_dir_config, srv)));

      assert (cx != nullptr);

      request rq (r);
      log lg (r->server, srv);
      return srv->template handle<M> (rq, cx, lg);
    }

    template <typename M>
    int service::
    handle (request& rq, const context* cx, log& lg) const
    {
      static const std::string func_name (
        "web::apache::service<" + name_ + ">::handle");

      try
      {
        auto i (exemplars_.find (cx));
        assert (i != exemplars_.end ());

        const M* e (dynamic_cast<const M*> (i->second.get ()));
        assert (e != nullptr);

        for (M m (*e);;)
        {
          try
          {
            if (static_cast<module&> (m).handle (rq, rq, lg))
              return rq.flush ();

            if (rq.state () == request_state::initial)
              return DECLINED;

            lg.write (nullptr, 0, func_name.c_str (), APLOG_ERR,
                      "handling declined being partially executed");
            break;
          }
          catch (const module::retry&)
          {
            // Retry to handle the request.
            //
            rq.rewind ();
          }
        }
      }
      catch (const invalid_request& e)
      {
        if (!e.content.empty () && rq.state () < request_state::writing)
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

        if (*e.what () && rq.state () < request_state::writing)
        {
          try
          {
            rq.content (
              HTTP_INTERNAL_SERVER_ERROR, "text/plain;charset=utf-8")
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

        if (rq.state () < request_state::writing)
        {
          try
          {
            rq.content (
              HTTP_INTERNAL_SERVER_ERROR, "text/plain;charset=utf-8")
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
