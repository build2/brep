// file      : mod/page.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_PAGE_HXX
#define MOD_PAGE_HXX

#include <libstudxml/forward.hxx>

#include <libbbot/manifest.hxx>

#include <web/xhtml/fragment.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <libbrep/build.hxx>
#include <libbrep/package.hxx>
#include <libbrep/review-manifest.hxx> // review_result

#include <mod/diagnostics.hxx>
#include <mod/options-types.hxx> // page_menu

namespace brep
{
  // Page common building blocks.
  //

  // Generate CSS link elements.
  //
  class CSS_LINKS
  {
  public:
    CSS_LINKS (const path& p, const dir_path& r): path_ (p), root_ (r) {}

    void
    operator() (xml::serializer&) const;

  private:
    const path& path_;
    const dir_path& root_;
  };

  // Generate page header element.
  //
  class DIV_HEADER
  {
  public:
    DIV_HEADER (const web::xhtml::fragment& logo,
                const vector<page_menu>& menu,
                const dir_path& root,
                const string& tenant):
        logo_ (logo), menu_ (menu), root_ (root), tenant_ (tenant) {}

    void
    operator() (xml::serializer&) const;

  private:
    const web::xhtml::fragment& logo_;
    const vector<page_menu>& menu_;
    const dir_path& root_;
    const string& tenant_;
  };

  // Generate package search form element with the specified query input
  // element name.
  //
  class FORM_SEARCH
  {
  public:
    FORM_SEARCH (const string& q, const string& n, bool a = true)
        : query_ (q), name_ (n), autofocus_ (a)
    {
    }

    void
    operator() (xml::serializer&) const;

  private:
    const string& query_;
    const string& name_;
    bool autofocus_;
  };

  // Generate counter element.
  //
  // If the count argument is nullopt, then it is assumed that the count is
  // unknown and the '?' character is printed instead of the number.
  //
  // Note that it could be redunant to distinguish between singular and plural
  // word forms if it wouldn't be so cheap in English, and phrase '1 Packages'
  // wouldn't look that ugly.
  //
  class DIV_COUNTER
  {
  public:
    DIV_COUNTER (optional<size_t> c, const char* s, const char* p)
        : count_ (c), singular_ (s), plural_ (p) {}

    void
    operator() (xml::serializer&) const;

  private:
    optional<size_t> count_;
    const char* singular_;
    const char* plural_;
  };

  // Generate table row element, that has the 'label: value' layout.
  //
  class TR_VALUE
  {
  public:
    TR_VALUE (const string& l, const string& v)
        : label_ (l), value_ (v) {}

    void
    operator() (xml::serializer&) const;

  private:
    const string& label_;
    const string& value_;
  };

  // Generate table row element, that has the 'label: <input type="text"/>'
  // layout.
  //
  class TR_INPUT
  {
  public:
    TR_INPUT (const string& l,
              const string& n,
              const string& v,
              const string& p = string (),
              bool a = false)
        : label_ (l),
          name_ (n),
          value_ (v),
          placeholder_ (!p.empty () ? &p : nullptr),
          autofocus_ (a)
    {
    }

    void
    operator() (xml::serializer&) const;

  private:
    const string& label_;
    const string& name_;
    const string& value_;
    const string* placeholder_;
    bool autofocus_;
  };

  // Generate table row element, that has the 'label: <select></select>'
  // layout. Option elements are represented as a list of value/inner-text
  // pairs.
  //
  class TR_SELECT
  {
  public:
    TR_SELECT (const string& l,
               const string& n,
               const string& v,
               const vector<pair<string, string>>& o)
        : label_ (l), name_ (n), value_ (v), options_ (o) {}

    void
    operator() (xml::serializer&) const;

  private:
    const string& label_;
    const string& name_;
    const string& value_;
    const vector<pair<string, string>>& options_;
  };

  // Generate tenant id element.
  //
  // Displays a link to the service page for the specified tenant.
  //
  class TR_TENANT
  {
  public:
    TR_TENANT (const string& n,
               const string& s,
               const dir_path& r,
               const string& t)
        : name_ (n), service_ (s), root_ (r), tenant_ (t) {}

    void
    operator() (xml::serializer&) const;

  private:
    const string& name_;
    const string& service_;
    const dir_path& root_;
    const string& tenant_;
  };

  // Generate package name element.
  //
  class TR_NAME
  {
  public:
    TR_NAME (const package_name& n, const dir_path& r, const string& t)
        : name_ (n), root_ (r), tenant_ (t) {}

    void
    operator() (xml::serializer&) const;

  private:
    const package_name& name_;
    const dir_path& root_;
    const string& tenant_;
  };

  // Generate package version element.
  //
  class TR_VERSION
  {
  public:
    // Display the version as a link to the package version details page.
    //
    TR_VERSION (const package_name& p,
                const version& v,
                const dir_path& r,
                const string& t,
                const optional<string>& u = nullopt)
        : package_ (&p),
          version_ (v.string ()),
          upstream_version_ (u ? &*u : nullptr),
          stub_ (v.compare (wildcard_version, true) == 0),
          root_ (&r),
          tenant_ (&t)
    {
    }

    // Display the version as a regular text.
    //
    TR_VERSION (const version& v, const optional<string>& u = nullopt)
        : package_ (nullptr),
          version_ (v.string ()),
          upstream_version_ (u ? &*u : nullptr),
          stub_ (v.compare (wildcard_version, true) == 0),
          root_ (nullptr),
          tenant_ (nullptr)
    {
    }

    void
    operator() (xml::serializer&) const;

  private:
    const package_name* package_;
    string version_;
    const string* upstream_version_;
    bool stub_;
    const dir_path* root_;
    const string* tenant_;
  };

  // Generate package project name element.
  //
  // Displays a link to the package search page with the project name
  // specified as a keyword.
  //
  class TR_PROJECT
  {
  public:
    TR_PROJECT (const package_name& p, const dir_path& r, const string& t)
        : project_ (p), root_ (r), tenant_ (t) {}

    void
    operator() (xml::serializer&) const;

  private:
    const package_name& project_;
    const dir_path& root_;
    const string& tenant_;
  };

  // Generate package summary element.
  //
  class TR_SUMMARY
  {
  public:
    TR_SUMMARY (const string& s): summary_ (s) {}

    void
    operator() (xml::serializer&) const;

  private:
    const string& summary_;
  };

  // Generate package license alternatives element.
  //
  class TR_LICENSE
  {
  public:
    TR_LICENSE (const license_alternatives& l): licenses_ (l) {}

    void
    operator() (xml::serializer&) const;

  private:
    const license_alternatives& licenses_;
  };

  // Generate package license alternatives elements. Differs from TR_LICENSE
  // by producing multiple rows instead of a single one.
  //
  class TR_LICENSES
  {
  public:
    TR_LICENSES (const license_alternatives& l): licenses_ (l) {}

    void
    operator() (xml::serializer&) const;

  private:
    const license_alternatives& licenses_;
  };

  // Generate package topics element.
  //
  class TR_TOPICS
  {
  public:
    TR_TOPICS (const small_vector<string, 5>& ts,
               const dir_path& r,
               const string& t)
        : topics_ (ts), root_ (r), tenant_ (t) {}

    void
    operator() (xml::serializer&) const;

  private:
    const small_vector<string, 5>& topics_;
    const dir_path& root_;
    const string& tenant_;
  };

  // Generate package dependencies element.
  //
  class TR_DEPENDS
  {
  public:
    TR_DEPENDS (const dependencies& d, const dir_path& r, const string& t)
        : dependencies_ (d), root_ (r), tenant_ (t) {}

    void
    operator() (xml::serializer&) const;

  private:
    const dependencies& dependencies_;
    const dir_path& root_;
    const string& tenant_;
  };

  // Generate package requirements element.
  //
  class TR_REQUIRES
  {
  public:
    TR_REQUIRES (const requirements& r): requirements_ (r) {}

    void
    operator() (xml::serializer&) const;

  private:
    const requirements& requirements_;
  };

  // Generate package versions reviews summary element.
  //
  class TR_REVIEWS_SUMMARY
  {
  public:
    TR_REVIEWS_SUMMARY (const optional<reviews_summary>& rs, const string& u)
        : reviews_ (rs), reviews_url_ (u) {}

    void
    operator() (xml::serializer&) const;

  private:
    const optional<reviews_summary>& reviews_;
    const string& reviews_url_;
  };

  // Generate package versions reviews summary counter element. The passed
  // review result denotes which kind of counter needs to be displayed and can
  // only be fail or pass.
  //
  class TR_REVIEWS_COUNTER
  {
  public:
    TR_REVIEWS_COUNTER (review_result r,
                        const optional<reviews_summary>& rs,
                        const string& u)
        : result (r),
          reviews_ (rs),
          reviews_url_ (u)
    {
      assert (r == review_result::fail || r == review_result::pass);
    }

    void
    operator() (xml::serializer&) const;

  private:
    review_result result;
    const optional<reviews_summary>& reviews_;
    const string& reviews_url_;
  };

  // Generate url element. Strip the `<scheme>://` prefix from the link text.
  //
  class TR_URL
  {
  public:
    TR_URL (const manifest_url& u, const char* l = "url")
        : url_ (u), label_ (l) {}

    void
    operator() (xml::serializer&) const;

  private:
    const manifest_url& url_;
    const char* label_;
  };

  // Generate email element.
  //
  class TR_EMAIL
  {
  public:
    TR_EMAIL (const email& e, const char* l = "email")
        : email_ (e), label_ (l) {}

    void
    operator() (xml::serializer&) const;

  private:
    const email& email_;
    const char* label_;
  };

  // Generate package version priority element.
  //
  class TR_PRIORITY
  {
  public:
    TR_PRIORITY (const priority& p): priority_ (p) {}

    void
    operator() (xml::serializer&) const;

  private:
    const priority& priority_;
  };

  // Generate repository name element.
  //
  class TR_REPOSITORY
  {
  public:
    TR_REPOSITORY (const repository_location& l,
                   const dir_path& r,
                   const string& t)
        : location_ (l), root_ (r), tenant_ (t) {}

    void
    operator() (xml::serializer&) const;

  private:
    const repository_location& location_;
    const dir_path& root_;
    const string& tenant_;
  };

  // Generate link element.
  //
  class TR_LINK
  {
  public:
    TR_LINK (const string& u, const string& t, const char* l)
        : url_ (u), text_ (t), label_ (l) {}

    void
    operator() (xml::serializer&) const;

  private:
    const string& url_;
    const string& text_;
    const char* label_;
  };

  // Generate sha256sum element.
  //
  class TR_SHA256SUM
  {
  public:
    TR_SHA256SUM (const string& s): sha256sum_ (s) {}

    void
    operator() (xml::serializer&) const;

  private:
    const string& sha256sum_;
  };

  // Generate build results element.
  //
  class TR_BUILD_RESULT
  {
  public:
    TR_BUILD_RESULT (const build& b,
                     bool a,
                     const string& h,
                     const dir_path& r):
        build_ (b), archived_ (a), host_ (h), root_ (r)
    {
      // We don't expect a queued build to ever be displayed.
      //
      assert (build_.state != build_state::queued);
    }

    void
    operator() (xml::serializer&) const;

  private:
    const build& build_;
    bool archived_;
    const string& host_;
    const dir_path& root_;
  };

  // Generate comment element.
  //
  class SPAN_COMMENT
  {
  public:
    SPAN_COMMENT (const string& c): comment_ (c) {}

    void
    operator() (xml::serializer&) const;

  private:
    const string& comment_;
  };

  // Generate package build result status element.
  //
  class SPAN_BUILD_RESULT_STATUS
  {
  public:
    SPAN_BUILD_RESULT_STATUS (const bbot::result_status& s): status_ (s) {}

    void
    operator() (xml::serializer&) const;

  private:
    const bbot::result_status& status_;
  };

  // Generate paragraph elements converting a plain text into XHTML5 applying
  // some heuristics (see implementation for details). Truncate the text if
  // requested.
  //
  // Note that there is no way to specify that some text fragment must stay
  // pre-formatted. Thus, don't use this type for text that can contain such
  // kind of fragments and consider using PRE_TEXT instead.
  //
  class P_TEXT
  {
  public:
    // Generate full text elements.
    //
    P_TEXT (const string& t, const string& id = "")
        : text_ (t), length_ (t.size ()), url_ (nullptr), id_ (id) {}

    // Generate brief text elements.
    //
    P_TEXT (const string& t, size_t l, const string& u, const string& id = "")
        : text_ (t), length_ (l), url_ (&u), id_ (id) {}

    void
    operator() (xml::serializer&) const;

  private:
    const string& text_;
    size_t length_;
    const string* url_; // Full page url.
    string id_;
  };

  // Generate pre-formatted text element. Truncate the text if requested.
  //
  class PRE_TEXT
  {
  public:
    // Generate a full text element.
    //
    PRE_TEXT (const string& t, const string& id = "")
        : text_ (t), length_ (t.size ()), url_ (nullptr), id_ (id) {}

    // Generate a brief text element.
    //
    PRE_TEXT (const string& t,
              size_t l,
              const string& u,
              const string& id = "")
        : text_ (t), length_ (l), url_ (&u), id_ (id) {}

    void
    operator() (xml::serializer&) const;

  private:
    const string& text_;
    size_t length_;
    const string* url_; // Full page url.
    string id_;
  };

  // Generate a typed text element truncating it if requested. On the
  // underlying parsing/rendering error, log it and generate the error
  // description element instead. Note that such an error indicates an issue
  // with the implementation, rather than with the specified text.
  //
  // Optionally strip the heuristically detected document "title". Currently,
  // this only applies to Markdown where a leading level-one heading is
  // assumed to be the title.
  //
  class DIV_TEXT
  {
  public:
    // Generate a full text element.
    //
    DIV_TEXT (const typed_text& t,
              bool st,
              const string& id,
              const string& what,
              const basic_mark& diag)
        : text_ (t),
          strip_title_ (st),
          length_ (t.text.size ()),
          url_ (nullptr),
          id_ (id),
          what_ (what),
          diag_ (diag)
    {
    }

    // Generate a brief text element.
    //
    DIV_TEXT (const typed_text& t,
              bool st,
              size_t l,
              const string& u,
              const string& id,
              const string& what,
              const basic_mark& diag)
        : text_ (t),
          strip_title_ (st),
          length_ (l),
          url_ (&u),
          id_ (id),
          what_ (what),
          diag_ (diag)
    {
    }

    void
    operator() (xml::serializer&) const;

  private:
    const typed_text& text_;
    bool strip_title_;
    size_t length_;
    const string* url_; // Full page url.
    string id_;
    const string& what_;
    const basic_mark& diag_;
  };

  // Generate paging element.
  //
  class DIV_PAGER
  {
  public:
    DIV_PAGER (size_t current_page,
               size_t item_count,
               size_t item_per_page,
               size_t page_number_count,
               const string& url);

    void
    operator() (xml::serializer&) const;

  private:
    size_t current_page_;
    size_t item_count_;
    size_t item_per_page_;
    size_t page_number_count_;
    const string& url_;
  };

  // Convert the argument to a string representing the valid HTML 5 'id'
  // attribute value.
  //
  string
  html_id (const string&);
}

#endif // MOD_PAGE_HXX
