// file      : web/server/mime-url-encoding.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef WEB_SERVER_MIME_URL_ENCODING_HXX
#define WEB_SERVER_MIME_URL_ENCODING_HXX

#include <string>

namespace web
{
  // URL-encode characters other than unreserved (see RFC3986). If the query
  // flag is true, then the encoding is applied to the URL query part, and so
  // convert space characters to plus characters rather than percent-encode
  // them.
  //
  std::string
  mime_url_encode (const char*, bool query = true);

  std::string
  mime_url_encode (const std::string&, bool query = true);

  // If the query flag is true, then convert plus characters to space
  // characters (see above). Throw std::invalid_argument if an invalid encoding
  // sequence is encountered.
  //
  std::string
  mime_url_decode (const char* b, const char* e,
                   bool trim = false,
                   bool query = true);
}

#endif // WEB_SERVER_MIME_URL_ENCODING_HXX
