// file      : mod/build-config.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/build-config.hxx>

#include <map>

#include <web/mime-url-encoding.hxx>

namespace brep
{
  using namespace web;
  using namespace bbot;

  shared_ptr<const build_configs>
  shared_build_config (const path& p)
  {
    static std::map<path, weak_ptr<build_configs>> configs;

    auto i (configs.find (p));
    if (i != configs.end ())
    {
      if (shared_ptr<build_configs> c = i->second.lock ())
        return c;
    }

    shared_ptr<build_configs> c (
      make_shared<build_configs> (parse_buildtab (p)));

    configs[p] = c;
    return c;
  }

  string
  build_log_url (const string& host, const dir_path& root,
                 const build& b,
                 const string* op)
  {
    // Note that '+' is the only package version character that potentially
    // needs to be url-encoded, and only in the query part of the URL. We embed
    // the package version into the URL path part and so don't encode it.
    //
    string url (host + root.representation () +
                mime_url_encode (b.package_name) + '/' +
                b.package_version.string () + "/log/" +
                mime_url_encode (b.configuration) + '/' +
                b.toolchain_version.string ());

    if (op != nullptr)
    {
      url += '/';
      url += *op;
    }

    return url;
  }

  string
  force_rebuild_url (const string& host, const dir_path& root, const build& b)
  {
    // Note that '+' is the only package version character that potentially
    // needs to be url-encoded, and only in the query part of the URL. However
    // we embed the package version into the URL query part, where it is not
    // encoded by design.
    //
    return host + root.string () +
      "?build-force&pn=" + mime_url_encode (b.package_name) +
      "&pv=" + b.package_version.string () +
      "&cf=" + mime_url_encode (b.configuration) +
      "&tc=" + b.toolchain_version.string () + "&reason=";
  }
}
