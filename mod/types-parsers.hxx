// file      : mod/types-parsers.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

// CLI parsers, included into the generated source files.
//

#ifndef MOD_TYPES_PARSERS_HXX
#define MOD_TYPES_PARSERS_HXX

#include <libbpkg/manifest.hxx> // repository_location

#include <web/xhtml/fragment.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/options-types.hxx>

namespace brep
{
  namespace cli
  {
    class scanner;

    template <typename T>
    struct parser;

    template <>
    struct parser<path>
    {
      static void
      parse (path&, bool&, scanner&);
    };

    template <>
    struct parser<dir_path>
    {
      static void
      parse (dir_path&, bool&, scanner&);
    };

    // Parse time of day specified in the `hh:mm` form.
    //
    template <>
    struct parser<duration>
    {
      static void
      parse (duration&, bool&, scanner&);
    };

    template <>
    struct parser<bpkg::repository_location>
    {
      static void
      parse (bpkg::repository_location&, bool&, scanner&);
    };

    template <>
    struct parser<page_form>
    {
      static void
      parse (page_form&, bool&, scanner&);
    };

    template <>
    struct parser<page_menu>
    {
      static void
      parse (page_menu&, bool&, scanner&);
    };

    template <>
    struct parser<web::xhtml::fragment>
    {
      static void
      parse (web::xhtml::fragment&, bool&, scanner&);
    };
  }
}

#endif // MOD_TYPES_PARSERS_HXX
