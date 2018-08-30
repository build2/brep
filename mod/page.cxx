// file      : mod/page.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/page.hxx>

#include <set>
#include <ios>       // hex, uppercase, right
#include <sstream>
#include <iomanip>   // setw(), setfill()
#include <algorithm> // min(), find()

#include <libstudxml/serializer.hxx>

#include <web/xhtml.hxx>
#include <web/mime-url-encoding.hxx>

#include <libbrep/package.hxx>
#include <libbrep/package-odb.hxx>

#include <mod/build-config.hxx> // build_log_url()

using namespace std;
using namespace xml;
using namespace web;
using namespace web::xhtml;

// Note that in HTML5 the boolean attribute absence represents false value,
// true otherwise. If it is present then the value must be empty or
// case-insensitively match the attribute's name.
//
namespace brep
{
  // CSS_LINKS
  //
  static const dir_path css_path ("@");

  void CSS_LINKS::
  operator() (serializer& s) const
  {
    s << *LINK(REL="stylesheet",
               TYPE="text/css",
               HREF=root_ / css_path / path_);
  }

  // DIV_HEADER
  //
  void DIV_HEADER::
  operator() (serializer& s) const
  {
    if (!logo_.empty () || !menu_.empty ())
    {
      s << DIV(ID="header-bar")
        <<   DIV(ID="header");

      if (!logo_.empty ())
        s << DIV(ID="header-logo") << logo_ << ~DIV;

      if (!menu_.empty ())
      {
        s << DIV(ID="header-menu")
          <<   DIV(ID="header-menu-body");

        for (const auto& m: menu_)
        {
          const string& l (m.link[0] == '/' || m.link.find (':') != string::npos
                           ? m.link
                           : root_.string () + m.link);

          s << A(HREF=l) << m.label << ~A;
        }

        s <<   ~DIV
          << ~DIV;
      }

      s <<   ~DIV
        << ~DIV;
    }
  }

  // FORM_SEARCH
  //
  void FORM_SEARCH::
  operator() (serializer& s) const
  {
    // The 'action' attribute is optional in HTML5. While the standard doesn't
    // specify browser behavior explicitly for the case the attribute is
    // omitted, the only reasonable behavior is to default it to the current
    // document URL.
    //
    s << FORM(ID="search")
      <<   TABLE(CLASS="form-table")
      <<     TBODY
      <<       TR
      <<         TD(ID="search-txt")
      <<           *INPUT(TYPE="search", NAME="q", VALUE=query_, AUTOFOCUS="")
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

  // TR_VALUE
  //
  void TR_VALUE::
  operator() (serializer& s) const
  {
    s << TR(CLASS=label_)
      <<   TH << label_ << ~TH
      <<   TD << SPAN(CLASS="value") << value_ << ~SPAN << ~TD
      << ~TR;
  }

  // TR_INPUT
  //
  void TR_INPUT::
  operator() (serializer& s) const
  {
    s << TR(CLASS=label_)
      <<   TH << label_ << ~TH
      <<   TD
      <<     INPUT(TYPE="text", NAME=name_);

    if (!value_.empty ())
      s << VALUE(value_);

    if (!placeholder_.empty ())
      s << PLACEHOLDER(placeholder_);

    if (autofocus_)
      s << AUTOFOCUS("");

    s <<     ~INPUT
      <<   ~TD
      << ~TR;
  }

  // TR_SELECT
  //
  void TR_SELECT::
  operator() (serializer& s) const
  {
    s << TR(CLASS=label_)
      <<   TH << label_ << ~TH
      <<   TD
      <<     SELECT(NAME=name_);

    for (const auto& o: options_)
    {
      s << OPTION(VALUE=o.first);

      if (o.first == value_)
        s << SELECTED("selected");

      s <<   o.second
        << ~OPTION;
    }

    s <<     ~SELECT
      <<   ~TD
      << ~TR;
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

      // Propagate search criteria to the package details page.
      //
      <<         root_ / path (mime_url_encode (name_.string (), false))
      <<         query_param_

      <<       ~HREF
      <<          name_
      <<       ~A
      <<     ~SPAN
      <<   ~TD
      << ~TR;
  }

  // TR_VERSION
  //
  void TR_VERSION::
  operator() (serializer& s) const
  {
    s << TR(CLASS="version")
      <<   TH << "version" << ~TH
      <<   TD
      <<     SPAN(CLASS="value");

    if (package_ == nullptr)
    {
      s << version_;

      if (stub_)
        s << " (stub)";
    }
    else
    {
      assert (root_ != nullptr);
      s << A(HREF=*root_ /
             dir_path (mime_url_encode (package_->string (), false)) /
             path (version_))
        <<   version_;

      if (stub_)
        s << " (stub)";

      s << ~A;
    }

    s <<     ~SPAN
      <<   ~TD
      << ~TR;
  }

  // TR_PROJECT
  //
  void TR_PROJECT::
  operator() (serializer& s) const
  {
    s << TR(CLASS="project")
      <<   TH << "project" << ~TH
      <<   TD
      <<     SPAN(CLASS="value")
      <<       A
      <<         HREF
      <<           root_ << "?q=" << mime_url_encode (project_.string ())
      <<         ~HREF
      <<         project_
      <<       ~A
      <<     ~SPAN
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
    if (!tags_.empty () || project_)
    {
      s << TR(CLASS="tags")
        <<   TH << "tags" << ~TH
        <<   TD
        <<     SPAN(CLASS="value");

      auto print = [&s, this] (const string& t)
      {
        s << A << HREF << root_ << "?q=" << mime_url_encode (t) << ~HREF
          <<   t
          << ~A;
      };

      bool pt (project_ != nullptr &&
               find (tags_.begin (), tags_.end (), *project_) == tags_.end ());

      if (pt)
        print (project_->string ());

      for (const string& t: tags_)
      {
        if (&t != &tags_[0] || pt)
          s << " ";

        print (t);
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

      if (d.buildtime)
        s << "*";

      // Suppress package name duplicates.
      //
      set<package_name> names;
      for (const auto& da: d)
        names.emplace (da.name);

      bool mult (names.size () > 1);

      if (mult)
        s << "(";

      bool first (true);
      for (const auto& da: d)
      {
        const package_name& n (da.name);
        if (names.find (n) != names.end ())
        {
          names.erase (n);

          if (first)
            first = false;
          else
            s << " | ";

          // Try to display the dependency as a link if it is resolved.
          // Otherwise display it as plain text.
          //
          if (da.package != nullptr)
          {
            shared_ptr<package> p (da.package.load ());
            assert (p->internal () || !p->other_repositories.empty ());

            shared_ptr<repository> r (
              p->internal ()
              ? p->internal_repository.load ()
              : p->other_repositories[0].load ());

            auto en (mime_url_encode (n.string (), false));

            if (r->interface_url)
              s << A(HREF=*r->interface_url + en) << n << ~A;
            else if (p->internal ())
              s << A(HREF=root_ / path (en)) << n << ~A;
            else
              // Display the dependency as plain text if no repository URL
              // available.
              //
              s << n;
          }
          else
            s << n;
        }
      }

      if (mult)
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

      if (r.buildtime)
        s << "*";

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
        bool mult (r.size () > 1);

        if (mult)
          s << "(";

        for (const auto& ra: r)
        {
          if (&ra != &r[0])
            s << " | ";

          s << ra;
        }

        if (mult)
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
      <<       A(HREF="mailto:" + email_) << email_ << ~A
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

  // TR_REPOSITORY
  //
  void TR_REPOSITORY::
  operator() (serializer& s) const
  {
    s << TR(CLASS="repository")
      <<   TH << "repository" << ~TH
      <<   TD
      <<     SPAN(CLASS="value")
      <<       A
      <<       HREF
      <<         root_ << "?about#" << mime_url_encode (html_id (name_), false)
      <<       ~HREF
      <<         name_
      <<       ~A
      <<     ~SPAN
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

  // TR_SHA256SUM
  //
  void TR_SHA256SUM::
  operator() (serializer& s) const
  {
    s << TR(CLASS="sha256")
      <<   TH << "sha256" << ~TH
      <<   TD << SPAN(CLASS="value") << sha256sum_ << ~SPAN << ~TD
      << ~TR;
  }

  // BUILD_RESULT
  //
  void TR_BUILD_RESULT::
  operator() (serializer& s) const
  {
    s << TR(CLASS="result")
      <<   TH << "result" << ~TH
      <<   TD
      <<     SPAN(CLASS="value");

    if (build_.state == build_state::building)
      s << SPAN(CLASS="building") << "building" << ~SPAN << " | ";
    else
    {
      // If no unsuccessful operation results available, then print the
      // overall build status. If there are any operation results available,
      // then also print unsuccessful operation statuses with the links to the
      // respective logs, followed with a link to the operation's combined
      // log. Print the forced package rebuild link afterwards, unless the
      // package build is already pending.
      //
      if (build_.results.empty () || *build_.status == result_status::success)
      {
        assert (build_.status);
        s << SPAN_BUILD_RESULT_STATUS (*build_.status) << " | ";
      }

      if (!build_.results.empty ())
      {
        for (const auto& r: build_.results)
        {
          if (r.status != result_status::success)
            s << SPAN_BUILD_RESULT_STATUS (r.status) << " ("
              << A
              <<   HREF
              <<     build_log_url (host_, root_, build_, &r.operation)
              <<   ~HREF
              <<   r.operation
              << ~A
              << ") | ";
        }

        s << A
          <<   HREF << build_log_url (host_, root_, build_) << ~HREF
          <<   "log"
          << ~A
          << " | ";
      }
    }

    if (build_.force == (build_.state == build_state::building
                         ? force_state::forcing
                         : force_state::forced))
      s << SPAN(CLASS="pending") << "pending" << ~SPAN;
    else
      s << A
        <<   HREF << force_rebuild_url (host_, root_, build_) << ~HREF
        <<   "rebuild"
        << ~A;

    s <<     ~SPAN
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
        <<   (comment_.back () == '.' ? string (comment_, 0, l - 1) : comment_)
        << ~SPAN;
  }

  // SPAN_BUILD_RESULT_STATUS
  //
  void SPAN_BUILD_RESULT_STATUS::
  operator() (serializer& s) const
  {
    s << SPAN(CLASS=to_string (status_)) << status_ << ~SPAN;
  }

  // P_DESCRIPTION
  //
  void P_DESCRIPTION::
  operator() (serializer& s) const
  {
    if (description_.empty ())
      return;

    auto n (description_.find_first_of (" \t\n", length_));
    bool full (n == string::npos); // Description length is below the limit.

    // Truncate description if length exceed the limit.
    //
    const string& d (full ? description_ : string (description_, 0, n));

    // Format the description into paragraphs, recognizing a blank line as
    // paragraph separator, and replacing single newlines with a space.
    //
    s << P;

    if (!id_.empty ())
      s << ID(id_);

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

    if (!full)
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

    auto n (changes_.find_first_of (" \t\n", length_));
    bool full (n == string::npos); // Changes length is below the limit.

    // Truncate changes if length exceed the limit.
    //
    const string& c (full ? changes_ : string (changes_, 0, n));
    s << PRE(ID="changes") << c;

    if (!full)
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

    size_t pcount (item_count_ / item_per_page_); // Page count.

    if (item_count_ % item_per_page_)
      ++pcount;

    if (pcount > 1)
    {
      auto url (
        [this](size_t page) -> string
        {
          return page == 0
            ? url_
            : url_ + (url_.find ('?') == string::npos ? "?p=" : "&p=") +
                to_string (page);
        });

      s << DIV(ID="pager");

      if (current_page_ > 0)
        s << A(ID="prev", HREF=url (current_page_ - 1)) << "Prev" << ~A;

      if (page_number_count_)
      {
        size_t offset (page_number_count_ / 2);
        size_t from (current_page_ > offset ? current_page_ - offset : 0);
        size_t to (min (from + page_number_count_, pcount));

        // Display as many pages as allowed.
        //
        if (to - from < page_number_count_ && from > 0)
          from -= min (from, page_number_count_ - (to - from));

        for (size_t p (from); p < to; ++p)
        {
          s << A(HREF=url (p));

          if (p == current_page_)
            s << ID("curr");

          s <<   p + 1
            << ~A;
        }
      }

      if (current_page_ < pcount - 1)
        s << A(ID="next", HREF=url (current_page_ + 1)) << "Next" << ~A;

      s << ~DIV;
    }
  }

  // Convert the argument to a string conformant to the section
  // "3.2.5.1 The id attribute" of the HTML 5 specification at
  // http://www.w3.org/TR/html5/dom.html#the-id-attribute.
  //
  string
  html_id (const string& v)
  {
    ostringstream o;
    o << hex << uppercase << right << setfill ('0');

    // Replace space characters (as specified at
    // http://www.w3.org/TR/html5/infrastructure.html#space-character) with
    // the respective escape sequences.
    //
    for (auto c: v)
    {
      switch (c)
      {
      case ' ':
      case '\t':
      case '\n':
      case '\r':
      case '\f':
      case '~':
        {
          // We use '~' as an escape character because it doesn't require
          // escaping in URLs.
          //
          o << "~" << setw (2) << static_cast<unsigned short> (c);
          break;
        }
      default: o << c; break;
      }
    }

    return o.str ();
  }
}
