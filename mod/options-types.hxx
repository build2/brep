// file      : mod/options-types.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_OPTIONS_TYPES_HXX
#define MOD_OPTIONS_TYPES_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

namespace brep
{
  // brep types
  //
  enum class page_form
  {
    full,
    brief
  };

  struct page_menu
  {
    string label;
    string link;

    page_menu () = default;
    page_menu (string b, string l): label (move (b)), link (move (l)) {}
  };
}

#endif // MOD_OPTIONS_TYPES_HXX
