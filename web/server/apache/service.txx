// file      : web/server/apache/service.txx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <httpd.h>    // APEXIT_CHILDSICK
#include <http_log.h> // APLOG_*

#include <cstdlib>   // exit()
#include <utility>   // move()
#include <exception>

#include <libbutl/utility.hxx> // operator<<(ostream, exception)

namespace web
{
  namespace apache
  {
    template <typename H>
    void service::
    init_worker (log& l)
    {
      using namespace std;

      const string func_name (
        "web::apache::service<" + name_ + ">::init_worker");

      try
      {
        const H* exemplar (dynamic_cast<const H*> (&exemplar_));
        assert (exemplar != nullptr);

        // For each directory configuration context, for which the handler is
        // allowed to handle a request, create the handler exemplar as a deep
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
                unique_ptr<handler> (new H (*exemplar))));

            r.first->second->init (o.second, l);
          }
        }

        // Options are not needed anymore. Free up the space.
        //
        options_.clear ();
      }
      catch (const exception& e)
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
        exit (APEXIT_CHILDSICK);
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
        exit (APEXIT_CHILDSICK);
      }
    }

    template <typename H>
    int service::
    request_handler (request_rec* r) noexcept
    {
      auto srv (instance<H> ());
      if (!r->handler || srv->name_ != r->handler) return DECLINED;

      assert (r->per_dir_config != nullptr);

      // Obtain the request-associated configuration context.
      //
      const context* cx (
        context_cast (ap_get_module_config (r->per_dir_config, srv)));

      assert (cx != nullptr);

      request rq (r);
      log lg (r->server, srv);
      return srv->template handle<H> (rq, cx, lg);
    }

    template <typename H>
    int service::
    handle (request& rq, const context* cx, log& lg) const
    {
      using namespace std;

      static const string func_name (
        "web::apache::service<" + name_ + ">::handle");

      try
      {
        auto i (exemplars_.find (cx));
        assert (i != exemplars_.end ());

        const H* e (dynamic_cast<const H*> (i->second.get ()));
        assert (e != nullptr);

        for (H h (*e);;)
        {
          try
          {
            if (static_cast<handler&> (h).handle (rq, rq, lg))
              return rq.flush ();

            if (rq.state () == request_state::initial)
              return DECLINED;

            lg.write (nullptr, 0, func_name.c_str (), APLOG_ERR,
                      "handling declined being partially executed");
            break;
          }
          catch (const handler::retry&)
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
            rq.content (e.status, e.type) << e.content << endl;
            return rq.flush ();
          }
          catch (const exception& e)
          {
            lg.write (nullptr, 0, func_name.c_str (), APLOG_ERR, e.what ());
          }
        }

        return e.status;
      }
      catch (const exception& e)
      {
        lg.write (nullptr, 0, func_name.c_str (), APLOG_ERR, e.what ());

        if (*e.what () && rq.state () < request_state::writing)
        {
          try
          {
            rq.content (
              HTTP_INTERNAL_SERVER_ERROR, "text/plain;charset=utf-8")
              << e << endl;

            return rq.flush ();
          }
          catch (const exception& e)
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
              << "unknown error" << endl;

            return rq.flush ();
          }
          catch (const exception& e)
          {
            lg.write (nullptr, 0, func_name.c_str (), APLOG_ERR, e.what ());
          }
        }
      }

      return HTTP_INTERNAL_SERVER_ERROR;
    }
  }
}
