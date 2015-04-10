// file      : brep/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <brep/module>

#include <functional> // bind()

using namespace std;

namespace brep
{

  void module::
  handle (request& rq, response& rs, log& l)
  {
    log_ = &l;

    try
    {
      handle (rq, rs);
    }
    catch (const invalid_request& e)
    {
      // @@ Both log and format as HTML in proper style, etc.
      //
      rs.content (e.status, "text/html;charset=utf-8") << e.description;
    }
    catch (const server_error& e)
    {
      // @@ Both log and return as 505.
      //
      write (move (e.data));
    }
    catch (const exception& e)
    {
      // @@ Exception: log e.what () & 505.
      //
      rs.status (505);
    }
    catch (...)
    {
      // @@ Unknown exception: log & 505.
      //
      rs.status (505);
    }
  }

  module::
  module ()
      : error (severity::error, log_writer_),
        warn (severity::warn, log_writer_),
        info (severity::info, log_writer_),
        log_writer_ (bind (&module::write, this, _1))
  {
  }

  void module::
  log_write (diag_data&& d) const
  {
    if (log_ == nullptr)
      return; // No backend yet.

    //@@ Cast log_ to apache::log and write the records.
    //
  }
}
