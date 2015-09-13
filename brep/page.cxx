// file      : brep/page.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/page>

#include <utility>   // move()
#include <algorithm> // min()

#include <xml/serializer>

#include <web/xhtml>

using namespace std;
using namespace xml;
using namespace web::xhtml;

namespace brep
{
  // pager
  //
  pager::
  pager (size_t current_page,
         size_t item_count,
         size_t item_per_page,
         size_t page_number_count,
         get_url_type get_url)
      : current_page_ (current_page),
        item_count_ (item_count),
        item_per_page_ (item_per_page),
        page_number_count_ (page_number_count),
        get_url_ (move (get_url))
  {
  }

  void pager::
  operator() (serializer& s) const
  {
    if (item_count_ == 0 || item_per_page_ == 0)
      return;

    size_t pc (item_count_ / item_per_page_); // Page count.

    if (item_count_ % item_per_page_)
      ++pc;

    if (pc > 1)
    {
      // Can consider customizing class names if use-case appear.
      //
      s << DIV(CLASS="pager");

      if (current_page_ > 0)
        s << A(CLASS="pg-prev")
          << HREF << get_url_ (current_page_ - 1) << ~HREF
          <<   "<<"
          << ~A
          << " ";

      if (page_number_count_)
      {
        size_t offset (page_number_count_ / 2);
        size_t fp (current_page_ > offset ? current_page_ - offset : 0);
        size_t tp (min (fp + page_number_count_, pc));

        for (size_t p (fp); p < tp; ++p)
        {
          if (p == current_page_)
            s << SPAN(CLASS="pg-cpage")
              <<   p + 1
              << ~SPAN;
          else
            s << A(CLASS="pg-page")
              << HREF << get_url_ (p) << ~HREF
              <<   p + 1
              << ~A;

          s << " ";
        }
      }

      if (current_page_ < pc - 1)
        s << A(CLASS="pg-next")
          << HREF << get_url_ (current_page_ + 1) << ~HREF
          <<   ">>"
          << ~A;

      s << ~DIV;
    }
  }

  // pager_style
  //
  void pager_style::
  operator() (xml::serializer& s) const
  {
    const char* ident ("\n      ");
    s << ".pager {margin: 0.5em 0 0;}" << ident
      << ".pg-prev {padding: 0 0.3em 0 0;}" << ident
      << ".pg-page {padding: 0 0.3em 0 0;}" << ident
      << ".pg-cpage {padding: 0 0.3em 0 0; font-weight: bold;}";
  }
}
