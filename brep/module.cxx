// file      : brep/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <brep/module>

#include <functional> // bind()

using namespace std;
using namespace placeholders; // For std::bind's _1, etc.

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
    catch (server_error& e) // Non-const because of move() below.
    {
      // @@ Both log and return as 505.
      //
      log_write (move (e.data));
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
  module (): log_writer_ (bind (&module::log_write, this, _1)) {}

  void module::
  log_write (diag_data&& d) const
  {
    if (log_ == nullptr)
      return; // No backend yet.

    //@@ Cast log_ to apache::log and write the records.
    //

    //@@ __PRETTY_FUNCTION__ contains a lot of fluff that we probably
    // don't want in the logs (like return value and argument list;
    // though the argument list would distinguish between several
    // overloads). If that's the case, then this is probably the
    // best place to process the name and convert something like:
    //
    // void module::handle(request, response)
    //
    // To just:
    //
    // module::handle
    //
    // Note to someone who is going to implement this: searching for a
    // space to determine the end of the return type may not work if
    // the return type is, say, a template id or a pointer to function
    // type. It seems a more robust approach would be to scan backwards
    // until we find the first ')' -- this got to be the end of the
    // function argument list. Now we continue scanning backwards keeping
    // track of the ')' vs '(' balance (arguments can also be of pointer
    // to function type). Once we see an unbalanced '(', then we know this
    // is the beginning of the argument list. Everything between it and
    // the preceding space is the qualified function name. Good luck ;-).
    //
    // If we also use the name in handle() above (e.g., to return to
    // the user as part of 505), then we should do it there as well
    // (in which case factoring this functionality into a separate
    // function seem to make a lot of sense).
    //
  }
}
