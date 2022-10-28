// file      : libbrep/utility.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBREP_UTILITY_HXX
#define LIBBREP_UTILITY_HXX

#include <memory>    // make_shared()
#include <string>    // to_string()
#include <utility>   // move(), forward(), declval(), make_pair()
#include <cassert>   // assert()
#include <iterator>  // make_move_iterator()
#include <algorithm> // *

#include <libbutl/utility.hxx> // icasecmp(), reverse_iterate(),
                               // operator<<(ostream, exception)

namespace brep
{
  using std::move;
  using std::forward;
  using std::declval;

  using std::make_pair;
  using std::make_shared;
  using std::make_move_iterator;
  using std::to_string;

  // <libbutl/utility.hxx>
  //
  using butl::utf8;
  using butl::icasecmp;
  using butl::reverse_iterate;
}

#include <libbrep/version.hxx>

#endif // LIBBREP_UTILITY_HXX
