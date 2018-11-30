// file      : mod/build-config.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/build-config.hxx>

#include <map>
#include <sstream>
#include <algorithm> // replace()

#include <libbutl/sha256.mxx>
#include <libbutl/utility.mxx>    // throw_generic_error(), alpha(), etc.
#include <libbutl/openssl.mxx>
#include <libbutl/filesystem.mxx>

#include <web/mime-url-encoding.hxx>

#include <mod/utility.hxx>

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
      for (const dir_entry& de: dir_iterator (d, false /* ignore_dangling */))
      {
        if (de.path ().extension () == "pem" &&
            de.type () == entry_type::regular)
        {
          p = d / de.path ();

          openssl os (p, path ("-"), 2,
                      process_env (o.openssl (), o.openssl_envvar ()),
                      "pkey",
                      o.openssl_option (), "-pubin", "-outform", "DER");

          string fp (sha256 (os.in).string ());
          os.in.close ();

          if (!os.wait ())
            throw io_error ("");

          ak->emplace (move (fp), move (p));
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
    string url (host + tenant_dir (root, b.tenant).representation () +
                mime_url_encode (b.package_name.string (), false) + '/' +
                b.package_version.string () + "/log/" +
                mime_url_encode (b.configuration, false) + '/' +
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
    return host + tenant_dir (root, b.tenant).string () +
      "?build-force&pn=" + mime_url_encode (b.package_name.string ()) +
      "&pv=" + b.package_version.string () +
      "&cf=" + mime_url_encode (b.configuration) +
      "&tc=" + b.toolchain_version.string () + "&reason=";
  }

  bool
  exclude (const build_class_exprs& exprs,
           const build_constraints& constrs,
           const build_config& cfg,
           string* reason)
  {
    // Save the first sentence of the reason, lower-case the first letter if
    // the beginning looks like a word (the second character is the
    // lower-case letter or space).
    //
    auto sanitize = [] (const string& reason)
    {
      string r (reason.substr (0, reason.find ('.')));

      char c;
      size_t n (r.size ());

      if (n > 0            &&
          alpha (c = r[0]) &&
          c == ucase (c)   &&
          (n == 1 || (alpha (c = r[1]) && c == lcase (c)) || c == ' '))
        r[0] = lcase (r[0]);

      return r;
    };

    bool r (false);

    // First, match the configuration against the package underlying build
    // class set and expressions.
    //
    // Determine the underlying class set. Note that in the future we can
    // potentially extend the underlying set with the special classes.
    //
    build_class_expr ucs (
      !exprs.empty () && !exprs.front ().underlying_classes.empty ()
      ? exprs.front ()
      : build_class_expr ("default", "Default"));

    // Transform the combined package build configuration class expression,
    // making the underlying class set a starting set for the original
    // expression and a restricting set, simultaneously. For example, for the
    // expression:
    //
    // default legacy : -msvc
    //
    // the resulting expression will be:
    //
    // +default +legacy -msvc &( +default +legacy )
    //
    //
    build_class_exprs es;
    es.emplace_back (ucs.underlying_classes, '+', ucs.comment);
    es.insert       (es.end (), exprs.begin (), exprs.end ());
    es.emplace_back (ucs.underlying_classes, '&', ucs.comment);

    // We will use a comment of the first encountered excluding expression
    // (changing the result from true to false) or non-including one (leaving
    // the false result) as an exclusion reason.
    //
    for (const build_class_expr& e: es)
    {
      bool pr (r);
      e.match (cfg.classes, r);

      if (reason != nullptr)
      {
        // Reset the reason which, if saved, makes no sense anymore.
        //
        if (r)
        {
          reason->clear ();
        }
        else if (reason->empty () &&
                 //
                 // Exclusion.
                 //
                 (pr              ||
                  //
                  // Non-inclusion. Make sure that the build class expression
                  // is empty or starts with an addition (+...).
                  //
                  e.expr.empty () ||
                  e.expr.front ().operation == '+'))
        {
          *reason = sanitize (e.comment);
        }
      }
    }

    if (!r)
      return true;

    // Now check if the configuration is excluded/included via the patterns.
    //
    // To implement matching of absent name components with wildcard-only
    // pattern components we are going to convert names to paths (see
    // dash_components_to_path() for details).
    //
    // And if any of the build-{include,exclude} values (which is legal) or
    // the build configuration name/target (illegal) are invalid paths, then
    // we assume no match.
    //
    try
    {
      path cn (dash_components_to_path (cfg.name));
      path tg (dash_components_to_path (cfg.target.string ()));

      for (const build_constraint& c: constrs)
      {
        if (path_match (dash_components_to_path (c.config),
                        cn,
                        dir_path () /* start */,
                        path_match_flags::match_absent) &&
            (!c.target ||
             path_match (dash_components_to_path (*c.target),
                         tg,
                         dir_path () /* start */,
                         path_match_flags::match_absent)))
        {
          if (!c.exclusion)
            return false;

          if (reason != nullptr)
            *reason = sanitize (c.comment);

          return true;
        }
      }
    }
    catch (const invalid_path&) {}

    return false;
  }

  path
  dash_components_to_path (const string& s)
  {
    string r;
    for (size_t i (0); i != s.size (); ++i)
    {
      char c (s[i]);

      switch (c)
      {
      case '-':
        {
          r += '/';
          break;
        }
      case '*':
        {
          if (s[i + 1] == '*') // Can be '\0'.
          {
            r += "*/**/*";
            ++i;
            break;
          }
        }
        // Fall through.
      default:
        {
          r += c;
          break;
        }
      }
    }

    return path (move (r));
  }
}
