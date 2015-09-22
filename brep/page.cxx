// file      : brep/page.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/page>

#include <string>
#include <cassert>
#include <utility>   // move()
#include <algorithm> // min()

#include <xml/serializer>

#include <web/xhtml>

#include <brep/package>

using namespace std;
using namespace xml;
using namespace web::xhtml;

namespace brep
{
  // A_STYLE
  //
  void A_STYLE::
  operator() (xml::serializer& s) const
  {
    const char* ident ("\n      ");
    s << "a {text-decoration: none;}" << ident
      << "a:hover {text-decoration: underline;}";
  }

  // DIV_PAGER
  //
  DIV_PAGER::
  DIV_PAGER (size_t current_page,
             size_t item_count,
             size_t item_per_page,
             size_t page_number_count,
             const string& url)
      : current_page_ (current_page),
        item_count_ (item_count),
        item_per_page_ (item_per_page),
        page_number_count_ (page_number_count),
        url_ (url)
  {
  }

  void DIV_PAGER::
  operator() (serializer& s) const
  {
    if (item_count_ == 0 || item_per_page_ == 0)
      return;

    size_t pc (item_count_ / item_per_page_); // Page count.

    if (item_count_ % item_per_page_)
      ++pc;

    if (pc > 1)
    {
      auto u (
        [this](size_t page) -> string
        {
          return page == 0
            ? url_
            : url_ + (url_.find ('?') == string::npos ? "?p=" : "&p=") +
            to_string (page);
        });

      // Can consider customizing class names if use-case appear.
      //
      s << DIV(CLASS="pager");

      if (current_page_ > 0)
        s << A(CLASS="pg-prev")
          << HREF << u (current_page_ - 1) << ~HREF
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
            s << SPAN(CLASS="pg-cpage") << p + 1 << ~SPAN;
          else
            s << A(CLASS="pg-page")
              << HREF << u (p) << ~HREF
              <<   p + 1
              << ~A;

          s << " ";
        }
      }

      if (current_page_ < pc - 1)
        s << A(CLASS="pg-next")
          << HREF << u (current_page_ + 1) << ~HREF
          <<   ">>"
          << ~A;

      s << ~DIV;
    }
  }

  // DIV_PAGER_STYLE
  //
  void DIV_PAGER_STYLE::
  operator() (xml::serializer& s) const
  {
    const char* ident ("\n      ");
    s << ".pager {margin: 0.5em 0 0;}" << ident
      << ".pg-prev {padding: 0 0.3em 0 0;}" << ident
      << ".pg-page {padding: 0 0.3em 0 0;}" << ident
      << ".pg-cpage {padding: 0 0.3em 0 0; font-weight: bold;}";
  }

  // DIV_LICENSES
  //
  void DIV_LICENSES::
  operator() (serializer& s) const
  {
    s << DIV(CLASS="licenses")
      <<  "Licenses: ";

    for (const auto& la: license_alternatives_)
    {
      if (&la != &license_alternatives_[0])
        s << " | ";

      for (const auto& l: la)
      {
        if (&l != &la[0])
          s << " & ";

        s << l;
      }
    }

    s << ~DIV;
  }

  // DIV_TAGS
  //
  void DIV_TAGS::
  operator() (serializer& s) const
  {
    if (!tags_.empty ())
    {
      s << DIV(CLASS="tags")
        <<   "Tags: ";

      for (const auto& t: tags_)
        s << t << " ";

      s << ~DIV;
    }
  }

  // DIV_PRIORITY
  //
  void DIV_PRIORITY::
  operator() (serializer& s) const
  {
    static const strings priority_names (
      {"low", "medium", "high", "security"});

    assert (priority_ < priority_names.size ());

    s << DIV(CLASS="priority")
      <<   "Priority: " << priority_names[priority_]
      << ~DIV;
  }
}
