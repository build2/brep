// file      : libbrep/common-traits.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBREP_COMMON_TRAITS_HXX
#define LIBBREP_COMMON_TRAITS_HXX

#include <string>
#include <cstddef> // size_t
#include <utility> // move()

#include <odb/pgsql/traits.hxx>

#include <libbpkg/package-name.hxx>

namespace odb
{
  namespace pgsql
  {
    template <>
    class value_traits<bpkg::package_name, id_string>:
      value_traits<std::string, id_string>
    {
    public:
      using value_type = bpkg::package_name;
      using query_type = bpkg::package_name;
      using image_type = details::buffer;

      using base_type = value_traits<std::string, id_string>;

      static void
      set_value (value_type& v,
                 const details::buffer& b,
                 std::size_t n,
                 bool is_null)
      {
        std::string s;
        base_type::set_value (s, b, n, is_null);
        v = !s.empty () ? value_type (std::move (s)) : value_type ();
      }

      static void
      set_image (details::buffer& b,
                 std::size_t& n,
                 bool& is_null,
                 const value_type& v)
      {
        base_type::set_image (b, n, is_null, v.string ());
      }
    };

    template <>
    struct type_traits<bpkg::package_name>
    {
      static const database_type_id db_type_id = id_string;

      struct conversion
      {
        static const char* to () {return "(?)::CITEXT";}
      };
    };
  }
}

#endif // LIBBREP_COMMON_TRAITS_HXX
