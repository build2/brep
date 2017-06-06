// file      : mod/build-config.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/build-config.hxx>

#include <map>
#include <sstream>

#include <libbutl/sha256.hxx>
#include <libbutl/utility.hxx>    // throw_generic_error()
#include <libbutl/openssl.hxx>
#include <libbutl/filesystem.hxx>

#include <web/mime-url-encoding.hxx>

namespace brep
{
  using namespace std;
  using namespace web;
  using namespace butl;
  using namespace bbot;

  shared_ptr<const build_configs>
  shared_build_config (const path& p)
  {
    static map<path, weak_ptr<build_configs>> configs;

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

  shared_ptr<const bot_agent_keys>
  shared_bot_agent_keys (const options::openssl_options& o, const dir_path& d)
  {
    static map<dir_path, weak_ptr<bot_agent_keys>> keys;

    auto i (keys.find (d));
    if (i != keys.end ())
    {
      if (shared_ptr<bot_agent_keys> k = i->second.lock ())
        return k;
    }

    shared_ptr<bot_agent_keys> ak (make_shared<bot_agent_keys> ());

    // Intercept exception handling to make error descriptions more
    // informative.
    //
    // Path of the key being converted. Used for diagnostics.
    //
    path p;

    try
    {
      for (const dir_entry& de: dir_iterator (d))
      {
        if (de.path ().extension () == "pem" &&
            de.type () == entry_type::regular)
        {
          p = d / de.path ();

          openssl os (p, path ("-"), 2,
                      process_env (o.openssl (), o.openssl_envvar ()),
                      "pkey",
                      o.openssl_option (), "-pubin", "-outform", "DER");

          vector<char> k (os.in.read_binary ());
          os.in.close ();

          if (!os.wait ())
            throw io_error ("");

          ak->emplace (sha256 (k.data (), k.size ()).string (), move (p));
        }
      }
    }
    catch (const io_error&)
    {
      ostringstream os;
      os << "unable to convert bbot agent pubkey " << p;
      throw_generic_error (EIO, os.str ().c_str ());
    }
    catch (const process_error& e)
    {
      ostringstream os;
      os << "unable to convert bbot agent pubkey " << p;
      throw_generic_error (e.code ().value (), os.str ().c_str ());
    }
    catch (const system_error& e)
    {
      ostringstream os;
      os<< "unable to iterate over agents keys directory '" << d << "'";
      throw_generic_error (e.code ().value (), os.str ().c_str ());
    }

    keys[d] = ak;
    return ak;
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
