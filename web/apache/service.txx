// file      : web/apache/service.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <http_log.h>

#include <exception>

namespace web
{
  namespace apache
  {
    template <typename M>
    int service::
    handle (request& r, log& l) noexcept
    {
      static const std::string func_name (
        "web::apache::service<" + name_ + ">::handle");

      try
      {
        M m (static_cast<const M&> (exemplar_));
        static_cast<module&> (m).handle (r, r, l);
        return r.flush ();
      }
      catch (const invalid_request& e)
      {
        if (!e.content.empty () && !r.get_write_state ())
        {
          try
          {
            r.content (e.status, e.type) << e.content;
            return r.flush ();
          }
          catch (const std::exception& e)
          {
            l.write (nullptr, 0, func_name.c_str (), APLOG_ERR, e.what ());
          }
        }

        return e.status;
      }
      catch (const std::exception& e)
      {
        l.write (nullptr, 0, func_name.c_str (), APLOG_ERR, e.what ());

        if (*e.what () && !r.get_write_state ())
        {
          try
          {
            r.content (HTTP_INTERNAL_SERVER_ERROR, "text/plain;charset=utf-8")
              << e.what ();

            return r.flush ();
          }
          catch (const std::exception& e)
          {
            l.write (nullptr, 0, func_name.c_str (), APLOG_ERR, e.what ());
          }
        }
      }
      catch (...)
      {
        l.write (nullptr, 0, func_name.c_str (), APLOG_ERR, "unknown error");

        if (!r.get_write_state ())
        {
          try
          {
            r.content (HTTP_INTERNAL_SERVER_ERROR, "text/plain;charset=utf-8")
              << "unknown error";

            return r.flush ();
          }
          catch (const std::exception& e)
          {
            l.write (nullptr, 0, func_name.c_str (), APLOG_ERR, e.what ());
          }
        }
      }

      return HTTP_INTERNAL_SERVER_ERROR;
    }
  }
}
