// file      : mod/diagnostics.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/diagnostics.hxx>

using namespace std;
using namespace butl;

namespace brep
{
  diag_record::
  ~diag_record () noexcept(false)
  {
    // Don't flush the record if this destructor was called as part of
    // the stack unwinding.
    //
#ifdef __cpp_lib_uncaught_exceptions
    if (!data_.empty () && uncaught_ == uncaught_exceptions ())
#else
    // Fallback implementation. Right now this means we cannot use this
    // mechanism in destructors, which is not a big deal, except for one
    // place: exception_guard. Thus the ugly special check.
    //
    if (!data_.empty () &&
        (!uncaught_exception () || exception_unwinding_dtor ()))
#endif
    {
      data_.back ().msg = os_.str (); // Save last message.

      assert (epilogue_ != nullptr);
      (*epilogue_) (move (data_)); // Can throw.
    }
  }
}
