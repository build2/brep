// file      : libbrep/common.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBREP_COMMON_HXX
#define LIBBREP_COMMON_HXX

#include <ratio>
#include <chrono>
#include <type_traits> // static_assert

#include <odb/query.hxx>

#include <libbpkg/package-name.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

// The uint16_t value range is not fully covered by SMALLINT PostgreSQL type
// to which uint16_t is mapped by default.
//
#pragma db value(uint16_t) type("INTEGER")

namespace brep
{
  // Use an image type to map bpkg::version to the database since there
  // is no way to modify individual components directly.
  //
  #pragma db value
  struct _version
  {
    uint16_t epoch;
    string canonical_upstream;
    string canonical_release;
    optional<uint16_t> revision;
    string upstream;
    optional<string> release;
  };
}

#include <libbpkg/manifest.hxx>

namespace brep
{
  using optional_version = optional<bpkg::version>;
  using _optional_version = optional<_version>;
}

// Prevent assert() macro expansion in get/set expressions. This should
// appear after all #include directives since the assert() macro is
// redefined in each <assert.h> inclusion.
//
#ifdef ODB_COMPILER
#  undef assert
#  define assert assert
void assert (int);
#endif

// We have to keep these mappings at the global scope instead of inside
// the brep namespace because they need to be also effective in the
// bpkg namespace from which we "borrow" types (and some of them use version).
//
#pragma db map type(bpkg::version) as(brep::_version) \
  to(brep::_version{(?).epoch,                        \
                    (?).canonical_upstream,           \
                    (?).canonical_release,            \
                    (?).revision,                     \
                    (?).upstream,                     \
                    (?).release})                     \
  from(bpkg::version ((?).epoch,                      \
                      std::move ((?).upstream),       \
                      std::move ((?).release),        \
                      (?).revision,                   \
                      0))

#pragma db map type(brep::optional_version) as(brep::_optional_version) \
  to((?)                                                                \
     ? brep::_version{(?)->epoch,                                       \
                      (?)->canonical_upstream,                          \
                      (?)->canonical_release,                           \
                      (?)->revision,                                    \
                      (?)->upstream,                                    \
                      (?)->release}                                     \
     : brep::_optional_version ())                                      \
  from((?)                                                              \
       ? bpkg::version ((?)->epoch,                                     \
                        std::move ((?)->upstream),                      \
                        std::move ((?)->release),                       \
                        (?)->revision,                                  \
                        0)                                              \
       : brep::optional_version ())

namespace brep
{
  // path
  //
  #pragma db map type(path) as(string) to((?).string ()) from(brep::path (?))

  using optional_path = optional<path>;
  using optional_string = optional<string>;

  #pragma db map type(optional_path) as(brep::optional_string) \
    to((?) ? (?)->string () : brep::optional_string ())        \
    from((?) ? brep::path (*(?)) : brep::optional_path ())

  #pragma db map type(dir_path) as(string)     \
    to((?).string ()) from(brep::dir_path (?))

  // Make sure that timestamp can be represented in nonoseconds without loss
  // of accuracy, so the following ODB mapping is adequate.
  //
  static_assert(
    std::ratio_greater_equal<timestamp::period,
                             std::chrono::nanoseconds::period>::value,
    "The following timestamp ODB mapping is invalid");

  // As it pointed out in libbutl/timestamp.mxx we will overflow in year 2262,
  // but by that time some larger basic type will be available for mapping.
  //
  #pragma db map type(timestamp) as(uint64_t)                 \
    to(std::chrono::duration_cast<std::chrono::nanoseconds> ( \
         (?).time_since_epoch ()).count ())                   \
    from(brep::timestamp (                                    \
      std::chrono::duration_cast<brep::timestamp::duration> ( \
        std::chrono::nanoseconds (?))))

  // version
  //
  using bpkg::version;

  // Sometimes we need to split the version into two parts: the part
  // that goes into the object id (epoch, canonical upstream, canonical
  // release, revision) and the original upstream and release. This is what
  // the canonical_version and upstream_version value types are for. Note that
  // upstream_version derives from version and uses it as storage. The idea
  // here is this: when we split the version, we often still want to have the
  // "whole" version object readily accessible and that's exactly what this
  // strange contraption is for. See package for an example on how everything
  // fits together.
  //
  // Note that the object id cannot contain an optional member which is why we
  // make the revision type uint16_t and represent nullopt as zero. This
  // should be ok for package object ids referencing the package manifest
  // version values because an absent revision and zero revision mean the
  // same thing.
  //
  #pragma db value
  struct canonical_version
  {
    uint16_t epoch;
    string   canonical_upstream;
    string   canonical_release;
    uint16_t revision;

    canonical_version () = default;

    explicit
    canonical_version (const version& v)
        : epoch (v.epoch),
          canonical_upstream (v.canonical_upstream),
          canonical_release (v.canonical_release),
          revision (v.effective_revision ()) {}

    bool
    empty () const noexcept
    {
      // Note that an empty canonical_upstream doesn't denote an empty
      // canonical_version. Remeber, that canonical_upstream doesn't include
      // rightmost digit-only zero components? So non-empty version("0") has
      // an empty canonical_upstream.
      //
      return epoch == 0                  &&
             canonical_upstream.empty () &&
             canonical_release.empty ()  &&
             revision == 0;
    }

    // Change collation to ensure the proper comparison of the "absent" release
    // with a specified one.
    //
    // The default collation for UTF8-encoded TEXT columns in PostgreSQL is
    // UCA-compliant. This makes the statement 'a' < '~' to be false, which
    // in turn makes the statement 2.1-alpha < 2.1 to be false as well.
    //
    // Unicode Collation Algorithm (UCA): http://unicode.org/reports/tr10/
    //
    #pragma db member(canonical_release) options("COLLATE \"C\"")
  };

  #pragma db value transient
  struct upstream_version: version
  {
    #pragma db member(upstream_) virtual(string)                 \
      get(this.upstream)                                         \
      set(this = brep::version (                                 \
            0, std::move (?), std::string (), brep::nullopt, 0))

    #pragma db member(release_) virtual(optional_string)                    \
      get(this.release)                                                     \
      set(this = brep::version (                                            \
            0, std::move (this.upstream), std::move (?), brep::nullopt, 0))

    upstream_version () = default;
    upstream_version (version v): version (move (v)) {}
    upstream_version&
    operator= (version v) {version& b (*this); b = v; return *this;}

    void
    init (const canonical_version& cv, const upstream_version& uv)
    {
      // Note: revert the zero revision mapping (see above).
      //
      *this = version (cv.epoch,
                       uv.upstream,
                       uv.release,
                       (cv.revision != 0
                        ? optional<uint16_t> (cv.revision)
                        : nullopt),
                       0);

      assert (cv.canonical_upstream == canonical_upstream &&
              cv.canonical_release == canonical_release);
    }
  };

  // Wildcard version. Satisfies any dependency constraint and is represented
  // as 0+0 (which is also the "stub version"; since a real version is always
  // greater than the stub version, we reuse it to signify a special case).
  //
  extern const version wildcard_version;

  // package_name
  //
  using bpkg::package_name;

  #pragma db value(package_name) type("CITEXT")

  #pragma db map type("CITEXT") as("TEXT") to("(?)::CITEXT") from("(?)::TEXT")

  // package_id
  //
  #pragma db value
  struct package_id
  {
    string tenant;
    package_name name;
    canonical_version version;

    package_id () = default;
    package_id (string t, package_name n, const brep::version& v)
        : tenant (move (t)),
          name (move (n)),
          version (v) {}
  };

  // repository_type
  //
  using bpkg::repository_type;
  using bpkg::to_repository_type;

  #pragma db map type(repository_type) as(string) \
    to(to_string (?))                             \
    from(brep::to_repository_type (?))

  // repository_url
  //
  using bpkg::repository_url;

  #pragma db map type(repository_url) as(string)                            \
    to((?).string ())                                                       \
    from((?).empty () ? brep::repository_url () : brep::repository_url (?))

  // repository_location
  //
  using bpkg::repository_location;

  #pragma db value
  struct _repository_location
  {
    repository_url  url;
    repository_type type;
  };

  // Note that the type() call fails for an empty repository location.
  //
  #pragma db map type(repository_location) as(_repository_location) \
    to(brep::_repository_location {(?).url (),                      \
                                   (?).empty ()                     \
                                   ? brep::repository_type::pkg     \
                                   : (?).type ()})                  \
    from(brep::repository_location (std::move ((?).url), (?).type))

  // repository_id
  //
  #pragma db value
  struct repository_id
  {
    string tenant;
    string canonical_name;

    repository_id () = default;
    repository_id (string t, string n)
        : tenant (move (t)), canonical_name (move (n)) {}
  };

  // build_class_expr
  //
  using bpkg::build_class_expr;
  using build_class_exprs = small_vector<build_class_expr, 1>;

  #pragma db value(build_class_expr) definition

  #pragma db member(build_class_expr::expr) transient
  #pragma db member(build_class_expr::underlying_classes) transient

  #pragma db member(build_class_expr::expression) virtual(string) before \
    get(this.string ())                                                  \
    set(this = brep::build_class_expr ((?), "" /* comment */))

  // build_constraints
  //
  using bpkg::build_constraint;
  using build_constraints = vector<build_constraint>;

  #pragma db value(build_constraint) definition

  // The primary reason why a package is unbuildable by the build bot
  // controller service.
  //
  enum class unbuildable_reason: std::uint8_t
  {
    stub,          // A stub,                                     otherwise
    test,          // A separate test (built as part of primary), otherwise
    external,      // From an external repository,                otherwise
    unbuildable    // From an internal unbuildable repository.
  };

  string
  to_string (unbuildable_reason);

  unbuildable_reason
  to_unbuildable_reason (const string&); // May throw invalid_argument.

  inline ostream&
  operator<< (ostream& os, unbuildable_reason r) {return os << to_string (r);}

  using optional_unbuildable_reason = optional<unbuildable_reason>;

  #pragma db map type(unbuildable_reason) as(string) \
    to(to_string (?))                                \
    from(brep::to_unbuildable_reason (?))

  #pragma db map type(optional_unbuildable_reason) as(brep::optional_string) \
    to((?) ? to_string (*(?)) : brep::optional_string ())                    \
    from((?)                                                                 \
    ? brep::to_unbuildable_reason (*(?))                                     \
    : brep::optional_unbuildable_reason ())                                  \

  // Version comparison operators.
  //
  // They allow comparing objects that have epoch, canonical_upstream,
  // canonical_release, and revision data members. The idea is that this
  // works for both query members of types version and canonical_version.
  // Note, though, that the object revisions should be comparable (both
  // optional, numeric, etc), so to compare version to query member or
  // canonical_version you may need to explicitly convert the version object
  // to canonical_version.
  //
  template <typename T1, typename T2>
  inline auto
  compare_version_eq (const T1& x, const T2& y, bool revision)
    -> decltype (x.revision == y.revision)
  {
    // Since we don't quite know what T1 and T2 are (and where the resulting
    // expression will run), let's not push our luck with something like
    // (!revision || x.revision == y.revision).
    //
    auto r (x.epoch == y.epoch &&
            x.canonical_upstream == y.canonical_upstream &&
            x.canonical_release == y.canonical_release);

    return revision
      ? r && x.revision == y.revision
      : r;
  }

  template <typename T1, typename T2>
  inline auto
  compare_version_ne (const T1& x, const T2& y, bool revision)
    -> decltype (x.revision == y.revision)
  {
    auto r (x.epoch != y.epoch ||
            x.canonical_upstream != y.canonical_upstream ||
            x.canonical_release != y.canonical_release);

    return revision
      ? r || x.revision != y.revision
      : r;
  }

  template <typename T1, typename T2>
  inline auto
  compare_version_lt (const T1& x, const T2& y, bool revision)
    -> decltype (x.revision == y.revision)
  {
    auto r (
      x.epoch < y.epoch ||
      (x.epoch == y.epoch && x.canonical_upstream < y.canonical_upstream) ||
      (x.epoch == y.epoch && x.canonical_upstream == y.canonical_upstream &&
       x.canonical_release < y.canonical_release));

    return revision
      ? r ||
      (x.epoch == y.epoch && x.canonical_upstream == y.canonical_upstream &&
       x.canonical_release == y.canonical_release && x.revision < y.revision)
      : r;
  }

  template <typename T1, typename T2>
  inline auto
  compare_version_le (const T1& x, const T2& y, bool revision)
    -> decltype (x.revision == y.revision)
  {
    auto r (
      x.epoch < y.epoch ||
      (x.epoch == y.epoch && x.canonical_upstream < y.canonical_upstream));

    return revision
      ? r ||
      (x.epoch == y.epoch && x.canonical_upstream == y.canonical_upstream &&
       x.canonical_release < y.canonical_release) ||
      (x.epoch == y.epoch && x.canonical_upstream == y.canonical_upstream &&
       x.canonical_release == y.canonical_release && x.revision <= y.revision)
      : r ||
      (x.epoch == y.epoch && x.canonical_upstream == y.canonical_upstream &&
       x.canonical_release <= y.canonical_release);
  }

  template <typename T1, typename T2>
  inline auto
  compare_version_gt (const T1& x, const T2& y, bool revision)
    -> decltype (x.revision == y.revision)
  {
    auto r (
      x.epoch > y.epoch ||
      (x.epoch == y.epoch && x.canonical_upstream > y.canonical_upstream) ||
      (x.epoch == y.epoch && x.canonical_upstream == y.canonical_upstream &&
       x.canonical_release > y.canonical_release));

    return revision
      ? r ||
      (x.epoch == y.epoch && x.canonical_upstream == y.canonical_upstream &&
       x.canonical_release == y.canonical_release && x.revision > y.revision)
      : r;
  }

  template <typename T1, typename T2>
  inline auto
  compare_version_ge (const T1& x, const T2& y, bool revision)
    -> decltype (x.revision == y.revision)
  {
    auto r (
      x.epoch > y.epoch ||
      (x.epoch == y.epoch && x.canonical_upstream > y.canonical_upstream));

    return revision
      ? r ||
      (x.epoch == y.epoch && x.canonical_upstream == y.canonical_upstream &&
       x.canonical_release > y.canonical_release) ||
      (x.epoch == y.epoch && x.canonical_upstream == y.canonical_upstream &&
       x.canonical_release == y.canonical_release && x.revision >= y.revision)
      : r ||
      (x.epoch == y.epoch && x.canonical_upstream == y.canonical_upstream &&
       x.canonical_release >= y.canonical_release);
  }

  template <typename T>
  inline auto
  order_by_version_desc (
    const T& x,
    bool first = true) -> //decltype ("ORDER BY" + x.epoch)
                          decltype (x.epoch == 0)
  {
    return (first ? "ORDER BY" : ", ")
      + x.epoch + "DESC,"
      + x.canonical_upstream + "DESC,"
      + x.canonical_release + "DESC,"
      + x.revision + "DESC";
  }

  template <typename T>
  inline auto
  order_by_version (
    const T& x,
    bool first = true) -> //decltype ("ORDER BY" + x.epoch)
                          decltype (x.epoch == 0)
  {
    return (first ? "ORDER BY" : ", ")
      + x.epoch + ","
      + x.canonical_upstream + ","
      + x.canonical_release + ","
      + x.revision;
  }

  // Package id comparison operators.
  //
  inline bool
  operator< (const package_id& x, const package_id& y)
  {
    if (int r = x.tenant.compare (y.tenant))
      return r < 0;

    if (int r = x.name.compare (y.name))
      return r < 0;

    return compare_version_lt (x.version, y.version, true);
  }

  // They allow comparing objects that have tenant, name, and version data
  // members. The idea is that this works for both query members of package id
  // types (in particular in join conditions) as well as for values of
  // package_id type.
  //
  template <typename T1, typename T2>
  inline auto
  operator== (const T1& x, const T2& y)
    -> decltype (x.tenant == y.tenant &&
                 x.name == y.name     &&
                 x.version.epoch == y.version.epoch)
  {
    return x.tenant == y.tenant &&
           x.name == y.name     &&
           compare_version_eq (x.version, y.version, true);
  }

  template <typename T1, typename T2>
  inline auto
  operator!= (const T1& x, const T2& y)
    -> decltype (x.tenant == y.tenant &&
                 x.name == y.name     &&
                 x.version.epoch == y.version.epoch)
  {
    return x.tenant != y.tenant ||
           x.name != y.name     ||
           compare_version_ne (x.version, y.version, true);
  }

  // Allow comparing the query members with the query parameters bound by
  // reference to variables of the package id type (in particular in the
  // prepared queries).
  //
  // Note that it is not operator==() since the query template parameter type
  // can not be deduced from the function parameter types and needs to be
  // specified explicitly.
  //
  template <typename T, typename ID>
  inline auto
  equal (const ID& x, const package_id& y)
    -> decltype (x.tenant == odb::query<T>::_ref (y.tenant) &&
                 x.name == odb::query<T>::_ref (y.name)     &&
                 x.version.epoch == odb::query<T>::_ref (y.version.epoch))
  {
    using query = odb::query<T>;

    const auto& qv (x.version);
    const canonical_version& v (y.version);

    return x.tenant == query::_ref (y.tenant)                          &&
           x.name == query::_ref (y.name)                              &&
           qv.epoch == query::_ref (v.epoch)                           &&
           qv.canonical_upstream == query::_ref (v.canonical_upstream) &&
           qv.canonical_release == query::_ref (v.canonical_release)   &&
           qv.revision == query::_ref (v.revision);
  }

  // Repository id comparison operators.
  //
  inline bool
  operator< (const repository_id& x, const repository_id& y)
  {
    if (int r = x.tenant.compare (y.tenant))
      return r < 0;

    return x.canonical_name.compare (y.canonical_name) < 0;
  }

  // They allow comparing objects that have tenant and canonical_name data
  // members. The idea is that this works for both query members of repository
  // id types (in particular in join conditions) as well as for values of
  // repository_id type.
  //
  template <typename T1, typename T2>
  inline auto
  operator== (const T1& x, const T2& y)
    -> decltype (x.tenant == y.tenant && x.canonical_name == y.canonical_name)
  {
    return x.tenant == y.tenant && x.canonical_name == y.canonical_name;
  }

  template <typename T1, typename T2>
  inline auto
  operator!= (const T1& x, const T2& y)
    -> decltype (x.tenant == y.tenant && x.canonical_name == y.canonical_name)
  {
    return x.tenant != y.tenant || x.canonical_name != y.canonical_name;
  }
}

#endif // LIBBREP_COMMON_HXX
