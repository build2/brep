// file      : mod/build-config-module.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/build-config.hxx>

#include <libbutl/utility.mxx>      // alpha(), etc.
#include <libbutl/path-pattern.mxx>

namespace brep
{
  using namespace std;
  using namespace butl;
  using namespace bpkg;
  using namespace bbot;

  // The default underlying class set expression (see below).
  //
  static const build_class_expr default_ucs_expr (
    {"default"}, '+', "Default.");

  bool
  exclude (const small_vector<build_class_expr, 1>& exprs,
           const vector<build_constraint>& constrs,
           const build_config& cfg,
           const map<string, string>& class_inheritance_map,
           string* reason)
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
    auto match = [&cfg, &m, reason, &sanitize, &class_inheritance_map]
                 (const build_class_expr& e)
    {
      bool pm (m);
      e.match (cfg.classes, class_inheritance_map, m);

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

  path
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
