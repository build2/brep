// file      : web/xhtml-fragment.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef WEB_XHTML_FRAGMENT_HXX
#define WEB_XHTML_FRAGMENT_HXX

#include <string>
#include <vector>
#include <utility> // pair

#include <libstudxml/parser.hxx>
#include <libstudxml/forward.hxx>

namespace web
{
  namespace xhtml
  {
    // A parsed (via xml::parser) XHTML fragment that can later be serialized
    // to xml::serializer.
    //
    class fragment
    {
    public:
      bool truncated = false;

    public:
      fragment () = default;

      // Parse string as an XHTML document fragment, truncating it if
      // requested. The fragment should be complete, in the sense that all
      // elements should have closing tags. Elements and attributes are
      // considered to be in the namespace of the entire XHTML document, so no
      // namespace should be specified for them. Do not validate against XHTML
      // vocabulary. Can throw xml::parsing exception.
      //
      fragment (const std::string& xhtml,
                const std::string& input_name,
                size_t length = 0);

      void
      operator() (xml::serializer&) const;

      bool
      empty () const {return events_.empty ();}

    private:
      std::vector<std::pair<xml::parser::event_type, std::string>> events_;
    };
  }
}

#endif // WEB_XHTML_FRAGMENT_HXX
