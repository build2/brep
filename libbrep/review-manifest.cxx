// file      : libbrep/review-manifest.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbrep/review-manifest.hxx>

#include <libbutl/manifest-parser.hxx>
#include <libbutl/manifest-serializer.hxx>

using namespace std;
using namespace butl;

namespace brep
{
  using parser = manifest_parser;
  using parsing = manifest_parsing;
  using serializer = manifest_serializer;
  using serialization = manifest_serialization;
  using name_value = manifest_name_value;

  // review_result
  //
  string
  to_string (review_result r)
  {
    switch (r)
    {
    case review_result::pass:      return "pass";
    case review_result::fail:      return "fail";
    case review_result::unchanged: return "unchanged";
    }

    assert (false);
    return string ();
  }

  review_result
  to_review_result (const string& r)
  {
         if (r == "pass")      return review_result::pass;
    else if (r == "fail")      return review_result::fail;
    else if (r == "unchanged") return review_result::unchanged;
    else throw invalid_argument ("invalid review result '" + r + '\'');
  }

  // review_manifest
  //
  review_manifest::
  review_manifest (parser& p, bool iu)
      : review_manifest (p, p.next (), iu)
  {
    // Make sure this is the end.
    //
    name_value nv (p.next ());
    if (!nv.empty ())
      throw parsing (p.name (), nv.name_line, nv.name_column,
                     "single review manifest expected");
  }

  review_manifest::
  review_manifest (parser& p, name_value nv, bool iu)
  {
    auto bad_name ([&p, &nv](const string& d) {
        throw parsing (p.name (), nv.name_line, nv.name_column, d);});

    auto bad_value ([&p, &nv](const string& d) {
        throw parsing (p.name (), nv.value_line, nv.value_column, d);});

    // Make sure this is the start and we support the version.
    //
    if (!nv.name.empty ())
      throw parsing (p.name (), nv.name_line, nv.name_column,
                     "start of review manifest expected");

    if (nv.value != "1")
      throw parsing (p.name (), nv.value_line, nv.value_column,
                     "unsupported format version");

    bool need_base (false);
    bool need_details (false);

    for (nv = p.next (); !nv.empty (); nv = p.next ())
    {
      string& n (nv.name);
      string& v (nv.value);

      if (n == "reviewed-by")
      {
        if (!reviewed_by.empty ())
          bad_name ("reviewer redefinition");

        if (v.empty ())
          bad_value ("empty reviewer");

        reviewed_by = move (v);
      }
      else if (n.size () > 7 && n.compare (0, 7, "result-") == 0)
      {
        string name (n, 7, n.size () - 7);

        if (find_if (results.begin (), results.end (),
                     [&name] (const review_aspect& r)
                     {
                       return name == r.name;
                     }) != results.end ())
          bad_name (name + " review result redefinition");

        try
        {
          review_result r (to_review_result (v));

          if (r == review_result::fail)
            need_details = true;

          if (r == review_result::unchanged)
            need_base = true;

          results.push_back (review_aspect {move (name), r});
        }
        catch (const invalid_argument& e)
        {
          bad_value (e.what ());
        }
      }
      else if (n == "base-version")
      {
        if (base_version)
          bad_name ("base version redefinition");

        try
        {
          base_version = bpkg::version (v);
        }
        catch (const invalid_argument& e)
        {
          bad_value (e.what ());
        }
      }
      else if (n == "details-url")
      {
        if (details_url)
          bad_name ("details url redefinition");

        try
        {
          details_url = url (v);
        }
        catch (const invalid_argument& e)
        {
          bad_value (e.what ());
        }
      }
      else if (!iu)
        bad_name ("unknown name '" + n + "' in review manifest");
    }

    // Verify all non-optional values were specified.
    //
    if (reviewed_by.empty ())
      bad_value ("no reviewer specified");

    if (results.empty ())
      bad_value ("no result specified");

    if (!base_version && need_base)
      bad_value ("no base version specified");

    if (!details_url && need_details)
      bad_value ("no details url specified");
  }

  void review_manifest::
  serialize (serializer& s) const
  {
    // @@ Should we check that all non-optional values are specified and all
    //    values are valid?
    //
    s.next ("", "1"); // Start of manifest.

    auto bad_value ([&s](const string& d) {
        throw serialization (s.name (), d);});

    if (reviewed_by.empty ())
      bad_value ("empty reviewer");

    s.next ("reviewed-by", reviewed_by);

    for (const review_aspect& r: results)
      s.next ("result-" + r.name, to_string (r.result));

    if (base_version)
      s.next ("base-version", base_version->string ());

    if (details_url)
      s.next ("details-url", details_url->string ());

    s.next ("", ""); // End of manifest.
  }

  // review_manifests
  //
  review_manifests::
  review_manifests (parser& p, bool iu)
  {
    // Parse review manifests.
    //
    for (name_value nv (p.next ()); !nv.empty (); nv = p.next ())
      emplace_back (p, move (nv), iu);
  }

  void review_manifests::
  serialize (serializer& s) const
  {
    // Serialize review manifests.
    //
    for (const review_manifest& m: *this)
      m.serialize (s);

    s.next ("", ""); // End of stream.
  }
}
