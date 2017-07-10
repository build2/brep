// file      : web/mime-url-encoding.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef WEB_MIME_URL_ENCODING_HXX
#define WEB_MIME_URL_ENCODING_HXX

#include <string>
#include <iosfwd>

namespace web
{
  // @@ Add the query flag (true by default). If true, then the encoding is
  //    applied to the URL query part, and so the plus character is used to
  //    encode the space character. Audit use cases afterwards.
  //
  void
  mime_url_encode (const char* v, std::ostream& o);

  std::string
  mime_url_encode (const char* v);

  std::string
  mime_url_encode (const std::string& v);

  std::string
  mime_url_decode (const char* b, const char* e, bool trim = false);
}

#endif // WEB_MIME_URL_ENCODING_HXX