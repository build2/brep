// file      : mod/build-config-module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/build-config-module.hxx>

#include <errno.h> // EIO

#include <map>
#include <sstream>

#include <libbutl/sha256.mxx>
#include <libbutl/utility.mxx>    // throw_generic_error(), alpha(), etc.
#include <libbutl/openssl.mxx>
#include <libbutl/filesystem.mxx>

namespace brep
{
  using namespace std;
  using namespace butl;
  using namespace bpkg;
  using namespace bbot;

  // Return pointer to the shared build configurations instance, creating one
  // on the first call. Throw tab_parsing on parsing error, io_error on the
  // underlying OS error. Note: not thread-safe.
  //
  static shared_ptr<const build_configs>
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

  // Return pointer to the shared build bot agent public keys map, creating
  // one on the first call. Throw system_error on the underlying openssl or OS
  // error. Note: not thread-safe.
  //
  using bot_agent_key_map = map<string, path>;

  static shared_ptr<const bot_agent_key_map>
  shared_bot_agent_keys (const options::openssl_options& o, const dir_path& d)
  {
    static map<dir_path, weak_ptr<bot_agent_key_map>> keys;

    auto i (keys.find (d));
    if (i != keys.end ())
    {
      if (shared_ptr<bot_agent_key_map> k = i->second.lock ())
        return k;
    }

    shared_ptr<bot_agent_key_map> ak (make_shared<bot_agent_key_map> ());

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

  void build_config_module::
  init (const options::build& bo)
  {
    try
    {
      build_conf_ = shared_build_config (bo.build_config ());
    }
    catch (const io_error& e)
    {
      ostringstream os;
      os << "unable to read build configuration '" << bo.build_config ()
         << "': " << e;

      throw_generic_error (EIO, os.str ().c_str ());
    }

    if (bo.build_bot_agent_keys_specified ())
      bot_agent_key_map_ =
        shared_bot_agent_keys (bo, bo.build_bot_agent_keys ());

    cstrings conf_names;

    using conf_map_type = map<const char*,
                              const build_config*,
                              compare_c_string>;

    conf_map_type conf_map;

    for (const auto& c: *build_conf_)
    {
      const char* cn (c.name.c_str ());
      conf_map[cn] = &c;
      conf_names.push_back (cn);
    }

    build_conf_names_ = make_shared<cstrings> (move (conf_names));
    build_conf_map_ = make_shared<conf_map_type> (move (conf_map));
  }

  // The default underlying class set expression (see below).
  //
  static const build_class_expr default_ucs_expr (
    {"default"}, '+', "Default.");

  bool build_config_module::
  exclude (const small_vector<build_class_expr, 1>& exprs,
           const vector<build_constraint>& constrs,
           const build_config& cfg,
           string* reason) const
  {
    // Save the first sentence of the reason, lower-case the first letter if
    // the beginning looks like a word (all subsequent characters until a
    // whitespace are lower-case letters).
    //
    auto sanitize = [] (const string& reason)
    {
      string r (reason.substr (0, reason.find ('.')));

      char c (r[0]); // Can be '\0'.
      if (alpha (c) && c == ucase (c))
      {
        bool word (true);

        for (size_t i (1);
             i != r.size () && (c = r[i]) != ' ' && c != '\t' && c != '\n';
             ++i)
        {
          // Is not a word if contains a non-letter or an upper-case letter.
          //
          if (!alpha (c) || c == ucase (c))
          {
            word = false;
            break;
          }
        }

        if (word)
          r[0] = lcase (r[0]);
      }

      return r;
    };

    // First, match the configuration against the package underlying build
    // class set and expressions.
    //
    bool m (false);

    // Match the configuration against an expression, updating the match
    // result.
    //
    // We will use a comment of the first encountered excluding expression
    // (changing the result from true to false) or non-including one (leaving
    // the false result) as an exclusion reason.
    //
    auto match = [&cfg, &m, reason, &sanitize, this]
                 (const build_class_expr& e)
    {
      bool pm (m);
      e.match (cfg.classes, build_conf_->class_inheritance_map, m);

      if (reason != nullptr)
      {
        // Reset the reason which, if saved, makes no sense anymore.
        //
        if (m)
        {
          reason->clear ();
        }
        else if (reason->empty () &&
                 //
                 // Exclusion.
                 //
                 (pm              ||
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
    };

    // Determine the underlying class set. Note that in the future we can
    // potentially extend the underlying set with special classes.
    //
    const build_class_expr* ucs (
      !exprs.empty () && !exprs.front ().underlying_classes.empty ()
      ? &exprs.front ()
      : nullptr);

    // Note that the combined package build configuration class expression can
    // be represented as the underlying class set used as a starting set for
    // the original expressions and a restricting set, simultaneously. For
    // example, for the expression:
    //
    // default legacy : -msvc
    //
    // the resulting expression will be:
    //
    // +( +default +legacy ) -msvc &( +default +legacy )
    //
    // Let's, however, optimize it a bit based on the following facts:
    //
    // - If the underlying class set expression (+default +legacy in the above
    //   example) evaluates to false, then the resulting expression also
    //   evaluates to false due to the trailing '&' operation. Thus, we don't
    //   need to evaluate further if that's the case.
    //
    // - On the other hand, if the underlying class set expression evaluates
    //   to true, then we don't need to apply the trailing '&' operation as it
    //   cannot affect the result.
    //
    const build_class_expr& ucs_expr (
      ucs != nullptr
      ? build_class_expr (ucs->underlying_classes, '+', ucs->comment)
      : default_ucs_expr);

    match (ucs_expr);

    if (m)
    {
      for (const build_class_expr& e: exprs)
        match (e);
    }

    // Exclude the configuration if it doesn't match the compound expression.
    //
    if (!m)
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
    if (!constrs.empty ())
    try
    {
      path cn (dash_components_to_path (cfg.name));
      path tg (dash_components_to_path (cfg.target.string ()));

      for (const build_constraint& c: constrs)
      {
        if (path_match (cn,
                        dash_components_to_path (c.config),
                        dir_path () /* start */,
                        path_match_flags::match_absent) &&
            (!c.target ||
             path_match (tg,
                         dash_components_to_path (*c.target),
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

  bool build_config_module::
  belongs (const bbot::build_config& cfg, const char* cls) const
  {
    const map<string, string>& im (build_conf_->class_inheritance_map);

    for (const string& c: cfg.classes)
    {
      if (c == cls)
        return true;

      // Go through base classes.
      //
      for (auto i (im.find (c)); i != im.end (); )
      {
        const string& base (i->second);

        if (base == cls)
          return true;

        i = im.find (base);
      }
    }

    return false;
  }

  path build_config_module::
  dash_components_to_path (const string& pattern)
  {
    string r;
    size_t nstar (0);
    for (const path_pattern_term& pt: path_pattern_iterator (pattern))
    {
      switch (pt.type)
      {
      case path_pattern_term_type::star:
        {
          // Replace ** with */**/* and skip all the remaining stars that may
          // follow in this sequence.
          //
          if (nstar == 0)
            r += "*";
          else if (nstar == 1)
            r += "/**/*"; // The first star is already copied.

          break;
        }
      case path_pattern_term_type::literal:
        {
          // Replace '-' with '/' and fall through otherwise.
          //
          if (get_literal (pt) == '-')
          {
            r += '/';
            break;
          }
        }
        // Fall through.
      default:
        {
          r.append (pt.begin, pt.end); // Copy the pattern term as is.
        }
      }

      nstar = pt.star () ? nstar + 1 : 0;
    }

    // Append the trailing slash to match the resulting paths as directories.
    // This is required for the trailing /* we could append to match absent
    // directory path components (see path_match_flags::match_absent for
    // details).
    //
    // Note that valid dash components may not contain a trailing dash.
    // Anyway, any extra trailing slashes will be ignored by the path
    // constructor.
    //
    r += '/';

    return path (move (r));
  }
}
