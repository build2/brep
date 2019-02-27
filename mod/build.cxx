// file      : mod/build.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/build.hxx>

#include <web/mime-url-encoding.hxx>

#include <mod/utility.hxx>

namespace brep
{
  using namespace web;

  string
  build_log_url (const string& host, const dir_path& root,
                 const build& b,
                 const string* op)
  {
    // Note that '+' is the only package version character that potentially
    // needs to be url-encoded, and only in the query part of the URL. We embed
    // the package version into the URL path part and so don't encode it.
    //
    string url (host + tenant_dir (root, b.tenant).representation () +
                mime_url_encode (b.package_name.string (), false) + '/' +
                b.package_version.string () + "/log/" +
                mime_url_encode (b.configuration, false /* query */) + '/' +
                mime_url_encode (b.toolchain_name, false /* query */) + '/' +
                b.toolchain_version.string ());

    if (op != nullptr)
    {
      url += '/';
      url += *op;
    }

    return url;
  }

  string
  build_force_url (const string& host, const dir_path& root, const build& b)
  {
    // Note that '+' is the only package version character that potentially
    // needs to be url-encoded, and only in the query part of the URL. However
    // we embed the package version into the URL query part, where it is not
    // encoded by design.
    //
    return host + tenant_dir (root, b.tenant).string () +
      "?build-force&pn=" + mime_url_encode (b.package_name.string ()) +
      "&pv=" + b.package_version.string () +
      "&cf=" + mime_url_encode (b.configuration) +
      "&tn=" + mime_url_encode (b.toolchain_name) +
      "&tv=" + b.toolchain_version.string () +
      "&reason=";
  }
}
