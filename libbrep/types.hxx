// file      : libbrep/types.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBREP_TYPES_HXX
#define LIBBREP_TYPES_HXX

#include <vector>
#include <string>
#include <memory>        // unique_ptr, shared_ptr
#include <utility>       // pair
#include <cstddef>       // size_t, nullptr_t
#include <cstdint>       // uint{8,16,32,64}_t
#include <istream>
#include <ostream>
#include <functional>    // function, reference_wrapper

#include <ios>           // ios_base::failure
#include <exception>     // exception
#include <stdexcept>     // logic_error, invalid_argument, runtime_error
#include <system_error>

#include <odb/lazy-ptr.hxx>

#include <libbutl/path.mxx>
#include <libbutl/path-io.mxx>
#include <libbutl/optional.mxx>
#include <libbutl/timestamp.mxx>
#include <libbutl/small-vector.mxx>

namespace brep
{
  // Commonly-used types.
  //
  using std::uint8_t;
  using std::uint16_t;
  using std::uint32_t;
  using std::uint64_t;

  using std::size_t;
  using std::nullptr_t;

  using std::pair;
  using std::string;
  using std::function;
  using std::reference_wrapper;

  using std::unique_ptr;
  using std::shared_ptr;
  using std::weak_ptr;

  using std::vector;
  using butl::small_vector; // <libbutl/small-vector.mxx>

  using strings = vector<string>;
  using cstrings = vector<const char*>;

  using std::istream;
  using std::ostream;

  // Exceptions. While <exception> is included, there is no using for
  // std::exception -- use qualified.
  //
  using std::logic_error;
  using std::invalid_argument;
  using std::runtime_error;
  using std::system_error;
  using io_error = std::ios_base::failure;

  using std::generic_category;

  // <libbutl/optional.mxx>
  //
  using butl::optional;
  using butl::nullopt;

  // ODB smart pointers.
  //
  using odb::lazy_shared_ptr;
  using odb::lazy_weak_ptr;

  // <libbutl/path.mxx>
  //
  using butl::path;
  using butl::dir_path;
  using butl::basic_path;
  using butl::invalid_path;

  using paths = std::vector<path>;
  using dir_paths = std::vector<dir_path>;

  using butl::path_cast;

  // <libbutl/timestamp.mxx>
  //
  using butl::system_clock;
  using butl::timestamp;
  using butl::timestamp_nonexistent;
}

#endif // LIBBREP_TYPES_HXX
