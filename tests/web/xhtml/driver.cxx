// file      : tests/web/xhtml/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <iostream>
#include <functional>

#include <xml/serializer>

#include <web/xhtml>

using namespace std;
using namespace xml;

static bool
bad_sequence (const function<void(serializer& s)>& f)
{
  ostringstream o;
  serializer s (o, "osstream");

  try
  {
    f (s);
    return false;
  }
  catch (const serialization&)
  {
    return true;
  }
}

int
main ()
{
  using namespace web::xhtml;

  assert (bad_sequence ([](serializer& s) {s << HTML << ~HEAD;}));
  assert (bad_sequence ([](serializer& s) {s << HTML << DIV << ~P << ~HTML;}));
  assert (bad_sequence ([](serializer& s) {s << HTML << DIV << ~A << ~HTML;}));
  assert (bad_sequence ([](serializer& s) {s << P << A << "a" << ~P << ~P;}));
  assert (bad_sequence ([](serializer& s) {s << P << A << "a" << ~I << ~P;}));

  assert (
    bad_sequence (
      [](serializer& s) {s << P << A << ID << "A" << ~HREF << ~A << ~P;}));

  serializer s (cout, "output");

  s << HTML
    <<   HEAD
    <<     TITLE << "Example XHTML5 document" << ~TITLE
    <<   ~HEAD
    <<   BODY
    //
    // Inline elements (no indentation).
    //
    <<     P << "Here be " << B << "Dragons!" << ~B << *BR
    <<       "and a newline" << ~P
    //
    // Various ways to specify attributes.
    //
    <<     P(ID=123, CLASS="cool") << "Text" << ~P
    <<     P << (ID=123) << (CLASS="cool") << "Text" << ~P
    <<     P << ID(123) << CLASS("cool") << "Text" << ~P
    <<     P << ID << 123 << ~ID << CLASS << "cool" << ~CLASS << "Text" << ~P
    //
    // Empty element with attributes.
    //
    <<     P << "Text" << *BR(CLASS="double") << ~P
    <<   ~BODY
    << ~HTML;
}
