// file      : mod/page.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/page.hxx>

#include <cmark-gfm.h>
#include <cmark-gfm-extension_api.h>

#include <set>
#include <ios>      // hex, uppercase, right
#include <sstream>
#include <iomanip>  // setw(), setfill()
#include <iterator> // back_inserter()

#include <libstudxml/serializer.hxx>

#include <web/xhtml/fragment.hxx>
#include <web/xhtml/serialization.hxx>

#include <web/server/mime-url-encoding.hxx>

#include <libbrep/package.hxx>
#include <libbrep/package-odb.hxx>

#include <mod/build.hxx>   // build_log_url()
#include <mod/utility.hxx>

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
  static inline string
  label_to_class (const string& label)
  {
    if (label.find (' ') == string::npos)
      return label;

    string r;
    transform (label.begin (), label.end (),
               back_inserter (r),
               [] (char c) {return c != ' ' ? c : '-';});

    return r;
  }

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

        dir_path root (tenant_dir (root_, tenant_));

        for (const auto& m: menu_)
        {
          const string& l (
            m.link[0] == '/' || m.link.find (':') != string::npos
            ? m.link
            : root.string () + m.link);

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
      <<           INPUT(TYPE="search", NAME=name_, VALUE=query_);

    if (autofocus_)
      s << AUTOFOCUS("");

    s <<           ~INPUT
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
    s << DIV(ID="count");

    if (count_)
      s << *count_;
    else
      s << '?';

    s <<   ' '
      <<   (count_ && *count_ % 10 == 1 && *count_ % 100 != 11
            ? singular_
            : plural_)
      << ~DIV;
  }

  // TR_VALUE
  //
  void TR_VALUE::
  operator() (serializer& s) const
  {
    string c (label_to_class (label_));
    s << TR(CLASS=c)
      <<   TH << label_ << ~TH
      <<   TD << SPAN(CLASS="value") << value_ << ~SPAN << ~TD
      << ~TR;
  }

  // TR_INPUT
  //
  void TR_INPUT::
  operator() (serializer& s) const
  {
    string c (label_to_class (label_));
    s << TR(CLASS=c)
      <<   TH << label_ << ~TH
      <<   TD
      <<     INPUT(TYPE="text", NAME=name_);

    if (!value_.empty ())
      s << VALUE(value_);

    if (placeholder_ != nullptr)
      s << PLACEHOLDER(*placeholder_);

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
    string c (label_to_class (label_));
    s << TR(CLASS=c)
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

  // TR_TENANT
  //
  void TR_TENANT::
  operator() (serializer& s) const
  {
    s << TR(CLASS="tenant")
      <<   TH << name_ << ~TH
      <<   TD
      <<     SPAN(CLASS="value")
      <<       A
      <<       HREF << tenant_dir (root_, tenant_) << '?' << service_ << ~HREF
      <<          tenant_
      <<       ~A
      <<     ~SPAN
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
      <<         tenant_dir (root_, tenant_) /
                 path (mime_url_encode (name_.string (), false))
      <<       ~HREF
      <<         name_
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

      if (upstream_version_ != nullptr)
        s << " (" << *upstream_version_ << ')';
      else if (stub_)
        s << " (stub)";
    }
    else
    {
      assert (root_ != nullptr && tenant_ != nullptr);

      s << A(HREF=tenant_dir (*root_, *tenant_) /
             dir_path (mime_url_encode (package_->string (), false)) /
             path (version_))
        <<   version_
        << ~A;

      if (upstream_version_ != nullptr)
        s << " (" << *upstream_version_ << ')';
      else if (stub_)
        s << " (stub)";
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
      <<           tenant_dir (root_, tenant_) << "?packages="
      <<           mime_url_encode (project_.string ())
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

  // TR_TOPICS
  //
  void TR_TOPICS::
  operator() (serializer& s) const
  {
    // Omit the element if there are no topics.
    //
    if (topics_.empty ())
      return;

    s << TR(CLASS="topics")
      <<   TH << "topics" << ~TH
      <<   TD
      <<     SPAN(CLASS="value");

    for (const string& t: topics_)
    {
      s << A
      <<   HREF
      <<     tenant_dir (root_, tenant_) << "?packages="
      <<     mime_url_encode (t)
      <<   ~HREF
      <<   t
      << ~A;
    }

    s <<     ~SPAN
      <<   ~TD
      << ~TR;
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

    for (const dependency_alternatives& das: dependencies_)
    {
      if (&das != &dependencies_[0])
        s << ", ";

      if (das.buildtime)
        s << "*";

      // Suppress dependency alternative duplicates, like in
      // `{foo bar} < 1.1 | {foo bar} > 1.5`.
      //
      // Return the dependency package name space-separated list.
      //
      auto deps_list = [] (const dependency_alternative& da)
      {
        string r;
        for (const dependency& d: da)
        {
          if (!r.empty ())
            r += ' ';

          r += d.name.string ();
        }

        return r;
      };

      set<string> alternatives;
      for (const dependency_alternative& da: das)
        alternatives.insert (deps_list (da));

      // Note that we may end up with a single package name in parenthesis, if
      // its duplicates were suppresses. This, however, may be helpful,
      // indicating that there some alternatives for the package.
      //
      bool mult (das.size () > 1 ||
                 (das.size () == 1 && das[0].size () > 1));

      if (mult)
        s << '(';

      bool first (true);
      for (const dependency_alternative& da: das)
      {
        auto i (alternatives.find (deps_list (da)));

        if (i == alternatives.end ())
          continue;

        alternatives.erase (i);

        if (!first)
          s << " | ";
        else
          first = false;

        for (const dependency& d: da)
        {
          if (&d != &da[0])
            s << ' ';

          // Try to display the dependency as a link if it is resolved.
          // Otherwise display it as plain text.
          //
          const package_name& n (d.name);

          if (d.package != nullptr)
          {
            shared_ptr<package> p (d.package.load ());
            assert (p->internal () || !p->other_repositories.empty ());

            shared_ptr<repository> r (
              p->internal ()
              ? p->internal_repository.load ()
              : p->other_repositories[0].load ());

            auto en (mime_url_encode (n.string (), false));

            if (r->interface_url)
              s << A(HREF=*r->interface_url + en) << n << ~A;
            else if (p->internal ())
              s << A(HREF=tenant_dir (root_, tenant_) / path (en)) << n << ~A;
            else
              // Display the dependency as plain text if no repository URL
              // available.
              //
              s << n;
          }
          else
            s << n;
        }

        if (da.enable)
          s << " ?";
      }

      if (mult)
        s << ')';
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

    for (const auto& ras: requirements_)
    {
      if (&ras != &requirements_[0])
        s << ", ";

      if (ras.buildtime)
        s << '*';

      // If this is a simple requirement without id, then print the comment
      // first word.
      //
      if (ras.simple () && ras[0][0].empty ())
      {
        const auto& c (ras.comment);
        if (!c.empty ())
        {
          if (ras[0].enable)
            s << "? ";

          auto n (c.find (' '));
          s << string (c, 0, n);

          if (n != string::npos)
            s << "...";
        }
      }
      else
      {
        bool mult (ras.size () > 1 ||
                   (ras.size () == 1 && ras[0].size () > 1));

        if (mult)
          s << '(';

        for (const auto& ra: ras)
        {
          if (&ra != &ras[0])
            s << " | ";

          for (const string& r: ra)
          {
            if (&r != &ra[0])
              s << ' ';

            s << r;
          }

          if (ra.enable)
            s << " ?";
        }

        if (mult)
          s << ')';
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
    string c (label_to_class (label_));
    s << TR(CLASS=c)
      <<   TH << label_ << ~TH
      <<   TD
      <<     SPAN(CLASS="value");

    // Display HTTP(S) URL as link, striping the scheme prefix for the link
    // text. Display URL with a different scheme as plain text.
    //
    if (icasecmp (url_.scheme, "https") == 0 ||
        icasecmp (url_.scheme, "http") == 0)
    {
      url u (url_);
      u.scheme.clear ();

      s << A(HREF=url_) << u << ~A;
    }
    else
      s << url_;

    s <<     ~SPAN
      <<     SPAN_COMMENT (url_.comment)
      <<   ~TD
      << ~TR;
  }

  // TR_EMAIL
  //
  void TR_EMAIL::
  operator() (serializer& s) const
  {
    string c (label_to_class (label_));
    s << TR(CLASS=c)
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
  static const strings priority_names ({"medium", "high", "security"});

  void TR_PRIORITY::
  operator() (serializer& s) const
  {
    // Omit the element for low priority.
    //
    if (priority_ == priority::low)
      return;

    size_t p (priority_ - 1);

    assert (p < priority_names.size ());

    const string& pn (priority_names[p]);

    s << TR(CLASS="priority")
      <<   TH << "priority" << ~TH
      <<   TD
      <<     SPAN(CLASS="value " + pn) << pn << ~SPAN
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
      <<         tenant_dir (root_, tenant_) << "?about#"
      <<         mime_url_encode (html_id (location_.canonical_name ()), false)
      <<       ~HREF
      <<         location_
      <<       ~A
      <<     ~SPAN
      <<   ~TD
      << ~TR;
  }

  // TR_LINK
  //
  void TR_LINK::
  operator() (serializer& s) const
  {
    string c (label_to_class (label_));
    s << TR(CLASS=c)
      <<   TH << label_ << ~TH
      <<   TD
      <<     SPAN(CLASS="value") << A(HREF=url_) << text_ << ~A << ~SPAN
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

  // TR_BUILD_RESULT
  //
  void TR_BUILD_RESULT::
  operator() (serializer& s) const
  {
    s << TR(CLASS="result")
      <<   TH << "result" << ~TH
      <<   TD
      <<     SPAN(CLASS="value");

    // Print the ' | ' separator if this is not the first item and reset the
    // `first` flag to false otherwise.
    //
    bool first (true);
    auto separate = [&s, &first] ()
    {
      if (first)
        first = false;
      else
        s << " | ";
    };

    if (build_.state == build_state::building)
    {
      separate ();

      s << SPAN(CLASS="building") << "building" << ~SPAN;
    }
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

        separate ();

        s << SPAN_BUILD_RESULT_STATUS (*build_.status);
      }

      if (!build_.results.empty ())
      {
        for (const auto& r: build_.results)
        {
          if (r.status != result_status::success)
          {
            separate ();

            s << SPAN_BUILD_RESULT_STATUS (r.status) << " ("
              << A
              <<   HREF
              <<     build_log_url (host_, root_, build_, &r.operation)
              <<   ~HREF
              <<   r.operation
              << ~A
              << ")";
          }
        }

        separate ();

        s << A
          <<   HREF << build_log_url (host_, root_, build_) << ~HREF
          <<   "log"
          << ~A;
      }
    }

    if (!archived_)
    {
      separate ();

      if (build_.force == (build_.state == build_state::building
                           ? force_state::forcing
                           : force_state::forced))
        s << SPAN(CLASS="pending") << "pending" << ~SPAN;
      else
        s << A
          <<   HREF << build_force_url (host_, root_, build_) << ~HREF
          <<   "rebuild"
          << ~A;
    }

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

  // P_TEXT
  //
  void P_TEXT::
  operator() (serializer& s) const
  {
    if (text_.empty ())
      return;

    size_t n (text_.find_first_of (" \t\n", length_));
    bool full (n == string::npos); // Text length is below the limit.

    // Truncate the text if length exceeds the limit.
    //
    const string& t (full ? text_ : string (text_, 0, n));

    // Format the text into paragraphs, recognizing a blank line as paragraph
    // separator, and replacing single newlines with a space.
    //
    s << P;

    if (!id_.empty ())
      s << ID(id_);

    bool nl (false); // The previous character is '\n'.
    for (const char& c: t)
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

  // PRE_TEXT
  //
  static void
  serialize_pre_text (serializer& s,
                      const string& text,
                      size_t length,
                      const string* url,
                      const string& id)
  {
    if (text.empty ())
      return;

    size_t n (text.find_first_of (" \t\n", length));
    bool full (n == string::npos); // Text length is below the limit.

    // Truncate the text if length exceeds the limit.
    //
    const string& t (full ? text : string (text, 0, n));
    s << PRE;

    if (!id.empty ())
      s << ID(id);

    s << t;

    if (!full)
    {
      assert (url != nullptr);
      s << "... " << A(HREF=*url) << "More" << ~A;
    }

    s << ~PRE;
  }

  void PRE_TEXT::
  operator() (serializer& s) const
  {
    serialize_pre_text (s, text_, length_, url_, id_);
  }

  // DIV_TEXT
  //
  void DIV_TEXT::
  operator() (serializer& s) const
  {
    const string& t (text_.text);

    switch (text_.type)
    {
    case text_type::plain:
      {
        // To keep things regular we wrap the preformatted text into <div>.
        //
        s << DIV(ID=id_, CLASS="plain");
        serialize_pre_text (s, t, length_, url_, "" /* id */);
        s << ~DIV;
        break;
      }
    case text_type::common_mark:
    case text_type::github_mark:
      {
        // Convert Markdown into XHTML wrapping it into the <div> element.
        //
        auto print_error = [&s, this] (const string& e)
        {
          s << DIV(ID=id_, CLASS="markdown")
          <<   SPAN(CLASS="error") << e << ~SPAN
          << ~DIV;
        };

        // Note that the only possible reason for the following cmark API
        // calls to fail is the inability to allocate memory. Unfortunately,
        // instead of reporting the failure to the caller, the API issues
        // diagnostics to stderr and aborts the process. Let's decrease the
        // probability of such an event by limiting the text size to 1M.
        //
        if (t.size () > 1024 * 1024)
        {
          print_error (what_ + " is too long");
          return;
        }

        string html;
        {
          char* r;
          {
            // Parse Markdown into the AST.
            //
            // Note that the footnotes extension needs to be enabled via the
            // CMARK_OPT_FOOTNOTES flag rather than the
            // cmark_parser_attach_syntax_extension() function call.
            //
            unique_ptr<cmark_parser, void (*)(cmark_parser*)> parser (
              cmark_parser_new (CMARK_OPT_DEFAULT   |
                                CMARK_OPT_FOOTNOTES |
                                CMARK_OPT_VALIDATE_UTF8),
              [] (cmark_parser* p) {cmark_parser_free (p);});

            // Enable GitHub extensions in the parser, if requested.
            //
            if (text_.type == text_type::github_mark)
            {
              auto add = [&parser] (const char* ext)
              {
                cmark_syntax_extension* e (
                  cmark_find_syntax_extension (ext));

                // Built-in extension is only expected.
                //
                assert (e != nullptr);

                cmark_parser_attach_syntax_extension (parser.get (), e);
              };

              add ("table");
              add ("strikethrough");
              add ("autolink");
            }

            cmark_parser_feed (parser.get (), t.c_str (), t.size ());

            unique_ptr<cmark_node, void (*)(cmark_node*)> doc (
              cmark_parser_finish (parser.get ()),
              [] (cmark_node* n) {cmark_node_free (n);});

            // Strip the document "title".
            //
            if (strip_title_)
            {
              cmark_node* child (cmark_node_first_child (doc.get ()));

              if (child != nullptr                                  &&
                  cmark_node_get_type (child) == CMARK_NODE_HEADING &&
                  cmark_node_get_heading_level (child) == 1)
              {
                cmark_node_unlink (child);
                cmark_node_free (child);
              }
            }

            // Render the AST into an XHTML fragment.
            //
            // Note that unlike GitHub we follow the default API behavior and
            // don't allow the raw HTML in Markdown (omitting the
            // CMARK_OPT_UNSAFE flag). This way we can assume the rendered
            // HTML is a well-formed XHTML fragment, which we rely upon for
            // truncation (see below). Note that by default the renderer
            // suppresses any HTML-alike markup and unsafe URLs (javascript:,
            // etc).
            //
            r = cmark_render_html (doc.get (),
                                   CMARK_OPT_DEFAULT,
                                   nullptr /* extensions */);
          }

          unique_ptr<char, void (*)(char*)> deleter (
            r,
            [] (char* s) {cmark_get_default_mem_allocator ()->free (s);});

          html = r;
        }

        // From the CommonMark Spec it follows that the resulting HTML can be
        // assumed a well-formed XHTML fragment with all the elements having
        // closing tags. But let's not assume this being the case (due to some
        // library bug or similar) and handle the xml::parsing exception.
        //
        try
        {
          fragment f (html, "gfm-html", url_ == nullptr ? 0 : length_);

          s << DIV(ID=id_, CLASS="markdown");

          // Disable indentation not to introduce unwanted spaces.
          //
          s.suspend_indentation ();
          s << f;
          s.resume_indentation ();

          if (f.truncated)
            s << DIV(CLASS="more")
              <<   "... " << A(HREF=*url_) << "More" << ~A
              << ~DIV;

          s << ~DIV;
        }
        catch (const xml::parsing& e)
        {
          string error ("unable to parse " + what_ + " XHTML fragment: " +
                        e.what ());
          diag_ << error;
          print_error (error);
        }

        break;
      }
    }
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
