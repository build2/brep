// file      : brep/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <brep/module>

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
      // @@ Format as HTML in proper style.
      //
      rs.content (e.status, "text/html;charset=utf-8") << e.description;
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
}
