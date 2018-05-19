// file      : web/xhtml-fragment.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <web/xhtml-fragment.hxx>

#include <string>
#include <cassert>

#include <libstudxml/parser.hxx>
#include <libstudxml/serializer.hxx>

#include <web/xhtml.hxx>

using namespace std;
using namespace xml;

namespace web
{
  namespace xhtml
  {
    fragment::
    fragment (const string& text, const string& name)
    {
      // To parse the fragment make it a valid xml document, wrapping with the
      // root element.
      //
      string doc ("<d>" + text + "</d>");

      parser p (
        doc.c_str (),
        doc.size (),
        name,
        parser::receive_elements | parser::receive_characters |
        parser::receive_attributes_event);

      for (parser::event_type e: p)
      {
        switch (e)
        {
        case parser::start_element:
        case parser::start_attribute:
          {
            const auto& n (p.qname ());

            if (!n.namespace_ ().empty ())
              throw parsing (
                name, p.line (), p.column (), "namespace is not allowed");

            events_.emplace_back (e, n.name ());
            break;
          }
        case parser::end_element:
        case parser::end_attribute:
          {
            events_.emplace_back (e, "");
            break;
          }
        case parser::characters:
          {
            events_.emplace_back (e, p.value ());
            break;
          }
        default:
          assert (false);
        }
      }

      // Unwrap the fragment removing the root element events.
      //
      assert (events_.size () >= 2);
      events_.erase (events_.begin ());
      events_.pop_back ();
    }

    void fragment::
    operator() (serializer& s) const
    {
      for (const auto& e: events_)
      {
        switch (e.first)
        {
        case parser::start_element:
          {
            s.start_element (xmlns, e.second);
            break;
          }

        case parser::start_attribute:
          {
            s.start_attribute (e.second);
            break;
          }
        case parser::end_element:
          {
            s.end_element ();
            break;
          }
        case parser::end_attribute:
          {
            s.end_attribute ();
            break;
          }
        case parser::characters:
          {
            s.characters (e.second);
            break;
          }
        default:
          assert (false);
        }
      }
    }
  }
}
