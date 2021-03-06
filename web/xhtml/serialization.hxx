// file      : web/xhtml/serialization.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef WEB_XHTML_SERIALIZATION_HXX
#define WEB_XHTML_SERIALIZATION_HXX

#include <libstudxml/serializer.hxx>

#include <web/xhtml/version.hxx>

namespace web
{
  // "Canonical" XHTML5 vocabulary.
  //
  // * One-letter tag names and local variable clash problem
  //
  // a at|an|an  anc anch
  // b bt|bo|bl  bld bold
  // i it|it|it  itl ital
  // p pt|pr|pr  par para
  // q qt|qu|qt  quo quot
  // s st|st|st  stk strk
  // u ut|un|un  unl undr
  //
  // Other options:
  //   - _a, a_, xa
  //   - A, I
  //   - x::i
  //   - user-defined literals: "a"_e, "/a"_e, "id"_a
  //
  // Things can actually get much worse, consider:
  //
  // int i;
  // s << i << "text" << ~i;
  //
  // So perhaps this is the situation where the explicit namespace
  // qualification (e.g., x::p) is the only robust option?
  //
  //
  // * Element/attribute name clash problem (e.g., STYLE)
  //
  //   - some attribute/element name decorator (STYLEA, STYLE_A, STYLE_)
  //   - rename attribute/element (e.g., STYLEDEF or CSSSTYLE[adds TYPE]);
  //     in case of STYLE we should probably rename the element since
  //     attribute will be much more frequently used.
  //   - "scope" attributes inside elements (P::STYLE); somewhat
  //     burdensome: P(P::STYLE); could then use low-case names
  //     for attributes
  //   - "scope" elements inside other elements (HEAD::STYLE); also
  //     burdensome.
  //
  //
  // * Text wrapping/indentation
  //
  // For some (inline) elements we want additional indentation:
  //
  // 1. Indent content on newline (e.g., for <style>).
  // 2. Automatically wrap and indent lines at (or before) certain
  //    length, say, 80 characters (e.g., for <p>).
  //
  // Would be nice to be able to implement this at the XHTML level,
  // not XML.
  //
  namespace xhtml
  {
    const char* const xmlns = "http://www.w3.org/1999/xhtml";

    struct attr_value_base
    {
      const char* name;
      mutable const attr_value_base* next;

      virtual void
      operator() (xml::serializer& s) const = 0;

    protected:
      explicit
      attr_value_base (const char* n): name (n), next (nullptr) {}
    };

    template <typename T>
    struct attr_value: attr_value_base
    {
      const T* val;

      attr_value (const char* n, const T& v): attr_value_base (n), val (&v) {}

      virtual void
      operator() (xml::serializer& s) const
      {
        s.attribute (name, *val);
        if (next != nullptr)
          s << *next;
      }
    };

    struct element_base;

    // End tag of an element (~P).
    //
    struct end_element
    {
      const element_base* e;

      void
      operator() (xml::serializer& s) const;
    };

    // Element without any conten (*BR).
    //
    struct empty_element
    {
      const element_base* e;

      void
      operator() (xml::serializer& s) const;
    };

    struct element_base
    {
      virtual void
      start (xml::serializer& s) const = 0;

      virtual void
      end (xml::serializer& s) const = 0;

      void          operator() (xml::serializer& s) const {start (s);}
      end_element   operator~  () const {return end_element {this};}
      empty_element operator*  () const {return empty_element {this};}
    };

    inline void end_element::
    operator() (xml::serializer& s) const {e->end (s);}

    inline void empty_element::
    operator() (xml::serializer& s) const {s << *e << ~*e;}

    // Element with an attribute chain, e.g., P(ID = 123, CLASS = "abc").
    //
    struct attr_element: element_base
    {
      const element_base* e;
      const attr_value_base* a;

      attr_element (const element_base& e, const attr_value_base& a)
          : e (&e), a (&a) {}

      virtual void
      start (xml::serializer& s) const {e->start (s); s << *a;}

      virtual void
      end (xml::serializer& s) const {e->end (s);}
    };

    struct element: element_base
    {
      const char* name;

      explicit
      element (const char* n): name (n) {}

      virtual void
      start (xml::serializer& s) const {s.start_element (xmlns, name);}

      virtual void
      end (xml::serializer& s) const {s.end_element (xmlns, name);}

      // s << elem(attr1 = 123, attr2 = "abc");
      //
      template <typename T1>
      attr_element
      operator () (const attr_value<T1>& a1) const
      {
        return attr_element (*this, a1);
      }

      template <typename T1, typename... TN>
      attr_element
      operator () (const attr_value<T1>& a1, const attr_value<TN>&... an) const
      {
        a1.next = operator() (an...).a;
        return attr_element (*this, a1);
      }

      using element_base::operator();
    };

    struct inline_element: element
    {
      using element::element;

      virtual void
      start (xml::serializer& s) const
      {
        s.suspend_indentation ();
        element::start (s);
      }

      virtual void
      end (xml::serializer& s) const
      {
        element::end (s);
        s.resume_indentation ();
      }
    };

    struct attribute;
    struct end_attribute
    {
      const attribute* a;

      void
      operator() (xml::serializer& s) const;
    };

    struct attribute
    {
      const char* name;

      explicit
      attribute (const char* n): name (n) {}

      // s << (attr1 = 123) << (attr2 = "abc");
      //
      template <typename T>
      attr_value<T>
      operator= (const T& v) const {return attr_value<T> (name, v);}

      // s << attr1 (123) << attr2 ("abc");
      //
      template <typename T>
      attr_value<T>
      operator() (const T& v) const {return attr_value<T> (name, v);}

      // s << attr1 << 123 << ~attr1 << attr2 << "abc" << ~attr2;
      //
      virtual void
      start (xml::serializer& s) const {s.start_attribute (name);};

      virtual void
      end (xml::serializer& s) const {s.end_attribute (name);}

      void          operator() (xml::serializer& s) const {start (s);}
      end_attribute operator~  () const {return end_attribute {this};}
    };

    inline void end_attribute::
    operator() (xml::serializer& s) const {a->end (s);}

    // Elements.
    //
    // Note that they are all declared static which means we may end
    // up with multiple identical copies if this header get included
    // into multiple translation units. The hope here is that the
    // compiler will "see-through" and eliminate all of them.
    //
    struct html_element: element
    {
      html_element (): element ("html") {}

      virtual void
      start (xml::serializer& s) const
      {
        s.doctype_decl ("html");
        s.start_element (xmlns, name);
        s.namespace_decl (xmlns, "");
      }
    };
    static const html_element HTML;

    struct head_element: element
    {
      head_element (): element ("head") {}

      virtual void
      start (xml::serializer& s) const
      {
        s.start_element (xmlns, name);
        s.start_element (xmlns, "meta");
        s.attribute ("charset", "UTF-8");
        s.end_element ();
        s.start_element (xmlns, "meta");
        s.attribute ("name", "viewport");
        s.attribute ("content", "device-width, initial-scale=1");
        s.end_element ();
      }
    };
    static const head_element HEAD;

    struct css_style_element: element
    {
      css_style_element (): element ("style") {}

      virtual void
      start (xml::serializer& s) const
      {
        s.start_element (xmlns, name);
        s.attribute ("type", "text/css");
      }
    };
    static const css_style_element CSS_STYLE;

    static const element BODY     ("body");
    static const element DATALIST ("datalist");
    static const element DIV      ("div");
    static const element FORM     ("form");
    static const element H1       ("h1");
    static const element H2       ("h2");
    static const element H3       ("h3");
    static const element H4       ("h4");
    static const element H5       ("h5");
    static const element H6       ("h6");
    static const element LI       ("li");
    static const element LINK     ("link");
    static const element META     ("meta");
    static const element OPTION   ("option");
    static const element P        ("p");
    static const element PRE      ("pre");
    static const element SCRIPT   ("script");
    static const element SELECT   ("select");
    static const element TABLE    ("table");
    static const element TBODY    ("tbody");
    static const element TD       ("td");
    static const element TH       ("th");
    static const element TITLE    ("title");
    static const element TR       ("tr");
    static const element UL       ("ul");

    static const inline_element A     ("a");
    static const inline_element B     ("b");
    static const inline_element BR    ("br");
    static const inline_element CODE  ("code");
    static const inline_element EM    ("em");
    static const inline_element I     ("i");
    static const inline_element INPUT ("input");
    static const inline_element SPAN  ("span");
    static const inline_element U     ("u");

    // Attributes.
    //

    static const attribute AUTOFOCUS   ("autofocus");
    static const attribute CLASS       ("class");
    static const attribute CONTENT     ("content");
    static const attribute HREF        ("href");
    static const attribute ID          ("id");
    static const attribute LIST        ("list");
    static const attribute NAME        ("name");
    static const attribute REL         ("rel");
    static const attribute PLACEHOLDER ("placeholder");
    static const attribute SELECTED    ("selected");
    static const attribute STYLE       ("style");
    static const attribute TYPE        ("type");
    static const attribute VALUE       ("value");
  }
}

#endif // WEB_XHTML_SERIALIZATION_HXX
