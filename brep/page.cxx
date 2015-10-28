// file      : brep/page.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/page>

#include <set>
#include <string>
#include <cassert>
#include <utility>   // move()
#include <algorithm> // min()

#include <xml/serializer>

#include <web/xhtml>
#include <web/mime-url-encoding>

#include <brep/package>

using namespace std;
using namespace xml;
using namespace web;
using namespace web::xhtml;

namespace brep
{
  // CSS_LINKS
  //
  void CSS_LINKS::
  operator() (serializer& s) const
  {
    s << *LINK(REL="stylesheet", TYPE="text/css", HREF="/common.css")
      << *LINK(REL="stylesheet", TYPE="text/css", HREF=url_);
  }

  // DIV_HEADER
  //
  void DIV_HEADER::
  operator() (serializer& s) const
  {
    s << DIV(ID="header")
      <<   DIV(ID="header-menu")
      <<     A(HREF="/") << "packages" << ~A
      <<     A(HREF="/about") << "about" << ~A
      <<   ~DIV
      << ~DIV;
  }

  // FORM_SEARCH
  //
  void FORM_SEARCH::
  operator() (serializer& s) const
  {
    // The 'action' attribute is optional in HTML5. While the standard don't
    // specify browser behavior explicitly for the case the attribute is
    // ommited, the only reasonable behavior is to default it to the current
    // document URL.
    //
    s << FORM(ID="search")
      <<   TABLE(CLASS="form-table")
      <<     TBODY
      <<       TR
      <<         TD(ID="search-txt")
      <<           *INPUT(TYPE="search", NAME="q", VALUE=query_,
                          AUTOFOCUS="autofocus")
      <<         ~TD
      <<         TD(ID="search-btn")
      <<           *INPUT(TYPE="submit", VALUE="Search")
      <<         ~TD
      <<       ~TR
      <<     ~TBODY
      <<   ~TABLE
      << ~FORM;
  }

  // DIV_COUNTER
  //
  void DIV_COUNTER::
  operator() (serializer& s) const
  {
    s << DIV(ID="count")
      <<   count_ << " "
      <<   (count_ % 10 == 1 && count_ % 100 != 11 ? singular_ : plural_)
      << ~DIV;
  }

  // TR_NAME
  //
  void TR_NAME::
  operator() (serializer& s) const
  {
    s << TR(CLASS="name")
      <<   TH << "name" << ~TH
      <<   TD
      <<     SPAN(CLASS="value")
      <<       A
      <<       HREF
      <<         "/go/" << mime_url_encode (name_);

    // Propagate search criteria to the package details page.
    //
    if (!query_param_.empty ())
      s << "?" << query_param_;

    s <<       ~HREF
      <<          name_
      <<       ~A
      <<     ~SPAN
      <<   ~TD
      << ~TR;
  }

  void TR_VERSION::
  operator() (serializer& s) const
  {
    s << TR(CLASS="version")
      <<   TH << "version" << ~TH
      <<   TD
      <<     SPAN(CLASS="value");

    if (package_ == nullptr)
      s << version_;
    else
      s << A
        << HREF
        <<   "/go/" << mime_url_encode (*package_) << "/" << version_
        << ~HREF
        <<   version_
        << ~A;

    s <<     ~SPAN
      <<   ~TD
      << ~TR;
  }

  // TR_SUMMARY
  //
  void TR_SUMMARY::
  operator() (serializer& s) const
  {
    s << TR(CLASS="summary")
      <<   TH << "summary" << ~TH
      <<   TD << SPAN(CLASS="value") << summary_ << ~SPAN << ~TD
      << ~TR;
  }

  // TR_LICENSE
  //
  void TR_LICENSE::
  operator() (serializer& s) const
  {
    s << TR(CLASS="license")
      <<   TH << "license" << ~TH
      <<   TD
      <<     SPAN(CLASS="value");

      for (const auto& la: licenses_)
      {
        if (&la != &licenses_[0])
          s << " " << EM << "or" << ~EM << " ";

        bool m (la.size () > 1);

        if (m)
          s << "(";

        for (const auto& l: la)
        {
          if (&l != &la[0])
            s << " " << EM << "and" << ~EM << " ";

          s << l;
        }

        if (m)
          s << ")";
      }

    s <<     ~SPAN
      <<   ~TD
      << ~TR;
  }

  // TR_LICENSES
  //
  void TR_LICENSES::
  operator() (serializer& s) const
  {
    for (const auto& la: licenses_)
    {
      s << TR(CLASS="license")
        <<   TH << "license" << ~TH
        <<   TD
        <<     SPAN(CLASS="value");

      for (const auto& l: la)
      {
        if (&l != &la[0])
          s << " " << EM << "and" << ~EM << " ";

        s << l;
      }

      s <<     ~SPAN
        <<     SPAN_COMMENT (la.comment)
        <<   ~TD
        << ~TR;
    }
  }

  // TR_TAGS
  //
  void TR_TAGS::
  operator() (serializer& s) const
  {
    if (!tags_.empty ())
    {
      s << TR(CLASS="tags")
        <<   TH << "tags" << ~TH
        <<   TD
        <<     SPAN(CLASS="value");

      for (const auto& t: tags_)
      {
        if (&t != &tags_[0])
          s << " ";

        s << A << HREF << "/?q=" << mime_url_encode (t) << ~HREF << t << ~A;
      }

      s <<     ~SPAN
        <<   ~TD
        << ~TR;
    }
  }

  // TR_DEPENDS
  //
  void TR_DEPENDS::
  operator() (serializer& s) const
  {
    s << TR(CLASS="depends")
      <<   TH << "depends" << ~TH
      <<   TD
      <<     SPAN(CLASS="value")
      <<       dependencies_.size ();

    if (!dependencies_.empty ())
      s << "; ";

    for (const auto& d: dependencies_)
    {
      if (&d != &dependencies_[0])
        s << ", ";

      if (d.conditional)
        s << "?";

      // Suppress package name duplicates.
      //
      set<string> ds;
      for (const auto& da: d)
        ds.emplace (da.name);

      bool m (ds.size () > 1);

      if (m)
        s << "(";

      bool first (true);
      for (const auto& da: d)
      {
        if (ds.find (da.name) != ds.end ())
        {
          ds.erase (da.name);

          if (first)
            first = false;
          else
            s << " | ";

          s << da.name; // @@ Make it a link.
        }
      }

      if (m)
        s << ")";
    }

    s <<     ~SPAN
      <<   ~TD
      << ~TR;
  }

  // TR_REQUIRES
  //
  void TR_REQUIRES::
  operator() (serializer& s) const
  {
    // If there are no requirements, then we omit it, unlike depends, where we
    // show 0 explicitly.
    //
    if (requirements_.empty ())
      return;

    s << TR(CLASS="requires")
      <<   TH << "requires" << ~TH
      <<   TD
      <<     SPAN(CLASS="value")
      <<       requirements_.size () << "; ";

    for (const auto& r: requirements_)
    {
      if (&r != &requirements_[0])
        s << ", ";

      if (r.conditional)
        s << "?";

      if (r.empty ())
      {
        // If there is no requirement alternatives specified, then
        // print the comment first word.
        //
        const auto& c (r.comment);
        if (!c.empty ())
        {
          auto n (c.find (' '));
          s << string (c, 0, n);

          if (n != string::npos)
            s << "...";
        }
      }
      else
      {
        bool m (r.size () > 1);

        if (m)
          s << "(";

        for (const auto& ra: r)
        {
          if (&ra != &r[0])
            s << " | ";

          s << ra;
        }

        if (m)
          s << ")";
      }
    }

    s <<     ~SPAN
      <<   ~TD
      << ~TR;
  }

  // TR_URL
  //
  void TR_URL::
  operator() (serializer& s) const
  {
    s << TR(CLASS=label_)
      <<   TH << label_ << ~TH
      <<   TD
      <<     SPAN(CLASS="value") << A(HREF=url_) << url_ << ~A << ~SPAN
      <<     SPAN_COMMENT (url_.comment)
      <<   ~TD
      << ~TR;
  }

  // TR_EMAIL
  //
  void TR_EMAIL::
  operator() (serializer& s) const
  {
    s << TR(CLASS=label_)
      <<   TH << label_ << ~TH
      <<   TD
      <<     SPAN(CLASS="value")
      <<       A << HREF << "mailto:" << email_ << ~HREF << email_ << ~A
      <<     ~SPAN
      <<     SPAN_COMMENT (email_.comment)
      <<   ~TD
      << ~TR;
  }

  // TR_PRIORITY
  //
  void TR_PRIORITY::
  operator() (serializer& s) const
  {
    static const strings priority_names ({"low", "medium", "high", "security"});
    assert (priority_ < priority_names.size ());

    s << TR(CLASS="priority")
      <<   TH << "priority" << ~TH
      <<   TD
      <<     SPAN(CLASS="value") << priority_names[priority_] << ~SPAN
      <<     SPAN_COMMENT (priority_.comment)
      <<   ~TD
      << ~TR;
  }

  // TR_LOCATION
  //
  void TR_LOCATION::
  operator() (serializer& s) const
  {
    s << TR(CLASS="location")
      <<   TH << "location" << ~TH
      <<   TD << SPAN(CLASS="value") << location_ << ~SPAN << ~TD
      << ~TR;
  }

  // TR_DOWNLOAD
  //
  void TR_DOWNLOAD::
  operator() (serializer& s) const
  {
    s << TR(CLASS="download")
      <<   TH << "download" << ~TH
      <<   TD
      <<     SPAN(CLASS="value") << A(HREF=url_) << url_ << ~A << ~SPAN
      <<   ~TD
      << ~TR;
  }

  // SPAN_COMMENT
  //
  void SPAN_COMMENT::
  operator() (serializer& s) const
  {
    if (size_t l = comment_.size ())
      s << SPAN(CLASS="comment")
        <<   (comment_[l - 1] == '.' ? string (comment_, 0, l - 1) : comment_)
        << ~SPAN;
  }

  // P_DESCRIPTION
  //
  void P_DESCRIPTION::
  operator() (serializer& s) const
  {
    if (description_.empty ())
      return;

    string::size_type n (description_.find_first_of (" \t\n", length_));
    bool f (n == string::npos); // Description length is below the limit.

    // Truncate description if length exceed the limit.
    //
    const string& d (f ? description_ : string (description_, 0, n));

    // Format the description into paragraphs, recognizing a blank line as
    // paragraph separator, and replacing single newlines with a space.
    //
    s << P(ID="description");

    bool nl (false); // The previous character is '\n'.
    for (const auto& c: d)
    {
      if (c == '\n')
      {
        if (nl)
        {
          s << ~P << P;
          nl = false;
        }
        else
          nl = true; // Delay printing until the next character.
      }
      else
      {
        if (nl)
        {
          s << ' '; // Replace the previous newline with a space.
          nl = false;
        }

        s << c;
      }
    }

    if (!f)
    {
      assert (url_ != nullptr);
      s << "... " << A(HREF=*url_) << "More" << ~A;
    }

    s << ~P;
  }

  // PRE_CHANGES
  //
  void PRE_CHANGES::
  operator() (serializer& s) const
  {
    if (changes_.empty ())
      return;

    string::size_type n (changes_.find_first_of (" \t\n", length_));
    bool f (n == string::npos); // Changes length is below the limit.

    // Truncate changes if length exceed the limit.
    //
    const string& c (f ? changes_ : string (changes_, 0, n));
    s << PRE(ID="changes") << c;

    if (!f)
    {
      assert (url_ != nullptr);
      s << "... " << A(HREF=*url_) << "More" << ~A;
    }

    s << ~PRE;
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

      s << DIV(ID="pager");

      if (current_page_ > 0)
        s << A(ID="prev", HREF=u (current_page_ - 1)) << "Prev" << ~A;

      if (page_number_count_)
      {
        size_t offset (page_number_count_ / 2);
        size_t fp (current_page_ > offset ? current_page_ - offset : 0);
        size_t tp (min (fp + page_number_count_, pc));

        for (size_t p (fp); p < tp; ++p)
        {
          s << A(HREF=u (p));

          if (p == current_page_)
            s << ID << "curr" << ~ID;

          s <<   p + 1
            << ~A;
        }
      }

      if (current_page_ < pc - 1)
        s << A(ID="next", HREF=u (current_page_ + 1)) << "Next" << ~A;

      s << ~DIV;
    }
  }
}
