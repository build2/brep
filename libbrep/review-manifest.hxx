// file      : libbrep/review-manifest.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBREP_REVIEW_MANIFEST_HXX
#define LIBBREP_REVIEW_MANIFEST_HXX

#include <libbutl/manifest-forward.hxx>

#include <libbpkg/manifest.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

namespace brep
{
  enum class review_result: uint8_t
  {
    pass,
    fail,
    unchanged
  };

  string
  to_string (review_result);

  review_result
  to_review_result (const string&); // May throw invalid_argument.

  inline ostream&
  operator<< (ostream& os, review_result r)
  {
    return os << to_string (r);
  }

  struct review_aspect
  {
    string name;          // code, build, test, doc, etc
    review_result result;
  };

  class review_manifest
  {
  public:
    string reviewed_by;
    vector<review_aspect> results;
    optional<bpkg::version> base_version;
    optional<url> details_url;

    review_manifest (string r,
                     vector<review_aspect> rs,
                     optional<bpkg::version> bv,
                     optional<url> u)
        : reviewed_by (move (r)),
          results (move (rs)),
          base_version (move (bv)),
          details_url (move (u)) {}

  public:
    review_manifest () = default;
    review_manifest (butl::manifest_parser&, bool ignore_unknown = false);
    review_manifest (butl::manifest_parser&,
                     butl::manifest_name_value start,
                     bool ignore_unknown = false);

    void
    serialize (butl::manifest_serializer&) const;
  };

  class review_manifests: public vector<review_manifest>
  {
  public:
    review_manifests () = default;
    review_manifests (butl::manifest_parser&, bool ignore_unknown = false);

    void
    serialize (butl::manifest_serializer&) const;
  };
}

#endif // LIBBREP_REVIEW_MANIFEST_HXX
