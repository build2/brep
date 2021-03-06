// file      : web/server/apache/log.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef WEB_SERVER_APACHE_LOG_HXX
#define WEB_SERVER_APACHE_LOG_HXX

#include <httpd.h>       // request_rec, server_rec
#include <http_log.h>
#include <http_config.h> // module

#include <cstdint>   // uint64_t
#include <algorithm> // min()

#include <web/server/module.hxx>

namespace web
{
  namespace apache
  {
    class log: public web::log
    {
    public:

      log (server_rec* s, const ::module* m) noexcept
        : server_ (s), module_ (m) {}

      virtual void
      write (const char* msg) {write (APLOG_ERR, msg);}

      // Apache-specific interface.
      //
      void
      write (int level, const char* msg) const noexcept
      {
        write (nullptr, 0, nullptr, level, msg);
      }

      void
      write (const char* file,
             std::uint64_t line,
             const char* func,
             int level,
             const char* msg) const noexcept
      {
        if (file && *file)
          file = nullptr; // Skip file/line placeholder from log line.

        level = std::min (level, APLOG_TRACE8);

        if (func)
          ap_log_error (file,
                        line,
                        module_->module_index,
                        level,
                        0,
                        server_,
                        "[%s]: %s",
                        func,
                        msg);
        else
          // Skip function name placeholder from log line.
          //
          ap_log_error (file,
                        line,
                        module_->module_index,
                        level,
                        0,
                        server_,
                        ": %s",
                        msg);
      }

    private:
      server_rec* server_;
      const ::module* module_; // Apache module.
    };
  }
}

#endif // WEB_SERVER_APACHE_LOG_HXX
