// file      : brep/package-traits -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BREP_PACKAGE_TRAITS
#define BREP_PACKAGE_TRAITS

#include <cstddef> // size_t

#include <odb/pgsql/traits.hxx>

#include <libbrep/package.hxx> // weighted_text

namespace odb
{
  namespace pgsql
  {
    template <>
    class value_traits<brep::weighted_text, id_string>
    {
    public:
      typedef brep::weighted_text value_type;
      typedef value_type query_type;
      typedef details::buffer image_type;

      static void
      set_value (value_type&, const details::buffer&, std::size_t, bool) {}

      static void
      set_image (details::buffer&,
                 std::size_t& n,
                 bool& is_null,
                 const value_type&);
    };
  }
}

#endif // BREP_PACKAGE_TRAITS