// file      : mod/page.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_PAGE_HXX
#define MOD_PAGE_HXX

#include <libstudxml/forward.hxx>

#include <web/xhtml-fragment.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <libbrep/package.hxx>

#include <mod/options-types.hxx> // page_menu

namespace brep
{
  // Page common building blocks.
  //

  // Generates CSS link elements.
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

  // Generates page header element.
  //
  class DIV_HEADER
  {
  public:
    DIV_HEADER (const dir_path& root,
                const web::xhtml::fragment& logo,
                const vector<page_menu>& menu):
        root_ (root), logo_ (logo), menu_ (menu)  {}

    void
    operator() (xml::serializer&) const;

  private:
    const dir_path& root_;
    const web::xhtml::fragment& logo_;
    const vector<page_menu>& menu_;
  };

  // Generates package search form element.
  //
  class FORM_SEARCH
  {
  public:
    FORM_SEARCH (const string& q): query_ (q) {}

    void
    operator() (xml::serializer&) const;

  private:
    const string& query_;
  };

  // Generates counter element.
  //
  // It could be redunant to distinguish between singular and plural word forms
  // if it wouldn't be so cheap in English, and phrase '1 Packages' wouldn't
  // look that ugly.
  //
  class DIV_COUNTER
  {
  public:
    DIV_COUNTER (size_t c, const char* s, const char* p)
        : count_ (c), singular_ (s), plural_ (p) {}

    void
    operator() (xml::serializer&) const;

  private:
    size_t count_;
    const char* singular_;
    const char* plural_;
  };

  // Generates package name element.
  //
  class TR_NAME
  {
  public:
    TR_NAME (const string& n, const string& q, const dir_path& r)
        : name_ (n), query_param_ (q), root_ (r) {}

    void
    operator() (xml::serializer&) const;

  private:
    const string& name_;
    const string& query_param_;
    const dir_path& root_;
  };

  // Generates package version element.
  //
  class TR_VERSION
  {
  public:
    // Display the version as a link to the package version details page.
    //
    TR_VERSION (const string& p, const version& v, const dir_path& r)
        : package_ (&p),
          version_ (v.string ()),
          stub_ (v.compare (wildcard_version, true) == 0),
          root_ (&r)
    {
    }

    // Display the version as a regular text.
    //
    TR_VERSION (const version& v)
        : package_ (nullptr),
          version_ (v.string ()),
          stub_ (v.compare (wildcard_version, true) == 0),
          root_ (nullptr)
    {
    }

    void
    operator() (xml::serializer&) const;

  private:
    const string* package_;
    string version_;
    bool stub_;
    const dir_path* root_;
  };

  // Generates package summary element.
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

  // Generates package license alternatives element.
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

  // Generates package license alternatives elements. Differs from TR_LICENSE
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

  // Generates package tags element.
  //
  class TR_TAGS
  {
  public:
    TR_TAGS (const strings& ts, const dir_path& r): tags_ (ts), root_ (r) {}

    void
    operator() (xml::serializer&) const;

  private:
    const strings& tags_;
    const dir_path& root_;
  };

  // Generates package dependencies element.
  //
  class TR_DEPENDS
  {
  public:
    TR_DEPENDS (const dependencies& d, const dir_path& r)
        : dependencies_ (d), root_ (r) {}

    void
    operator() (xml::serializer&) const;

  private:
    const dependencies& dependencies_;
    const dir_path& root_;
  };

  // Generates package requirements element.
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

  // Generates url element.
  //
  class TR_URL
  {
  public:
    TR_URL (const url& u, const char* l = "url"): url_ (u), label_ (l) {}

    void
    operator() (xml::serializer&) const;

  private:
    const url& url_;
    const char* label_;
  };

  // Generates email element.
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

  // Generates package version priority element.
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

  // Generates repository name element.
  //
  class TR_REPOSITORY
  {
  public:
    TR_REPOSITORY (const string& n, const dir_path& r)
        : name_ (n), root_ (r) {}

    void
    operator() (xml::serializer&) const;

  private:
    const string& name_;
    const dir_path& root_;
  };

  // Generates repository location element.
  //
  class TR_LOCATION
  {
  public:
    TR_LOCATION (const repository_location& l): location_ (l) {}

    void
    operator() (xml::serializer&) const;

  private:
    const repository_location& location_;
  };

  // Generates package download URL element.
  //
  class TR_DOWNLOAD
  {
  public:
    TR_DOWNLOAD (const string& u): url_ (u) {}

    void
    operator() (xml::serializer&) const;

  private:
    const string& url_;
  };

  // Generates sha256sum element.
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

  // Generates comment element.
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

  // Generates package description element.
  //
  class P_DESCRIPTION
  {
  public:
    // Genereate full description.
    //
    P_DESCRIPTION (const string& d, const string& id = "")
        : description_ (d), length_ (d.size ()), url_ (nullptr), id_ (id) {}

    // Genereate brief description.
    //
    P_DESCRIPTION (const string& d, size_t l, const string& u)
        : description_ (d), length_ (l), url_ (&u) {}

    void
    operator() (xml::serializer&) const;

  private:
    const string& description_;
    size_t length_;
    const string* url_; // Full page url.
    string id_;
  };

  // Generates package description element.
  //
  class PRE_CHANGES
  {
  public:
    // Genereate full changes info.
    //
    PRE_CHANGES (const string& c)
        : changes_ (c), length_ (c.size ()), url_ (nullptr) {}

    // Genereate brief changes info.
    //
    PRE_CHANGES (const string& c, size_t l, const string& u)
        : changes_ (c), length_ (l), url_ (&u) {}

    void
    operator() (xml::serializer&) const;

  private:
    const string& changes_;
    size_t length_;
    const string* url_; // Full page url.
  };

  // Generates paging element.
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
