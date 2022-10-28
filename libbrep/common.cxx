// file      : libbrep/common.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbrep/common.hxx>

namespace brep
{
  const version wildcard_version (0, "0", nullopt, nullopt, 0);

  // unbuildable_reason
  //
  string
  to_string (unbuildable_reason r)
  {
    switch (r)
    {
    case unbuildable_reason::stub:        return "stub";
    case unbuildable_reason::test:        return "test";
    case unbuildable_reason::external:    return "external";
    case unbuildable_reason::unbuildable: return "unbuildable";
    }

    return string (); // Should never reach.
  }

  unbuildable_reason
  to_unbuildable_reason (const string& r)
  {
         if (r == "stub")        return unbuildable_reason::stub;
    else if (r == "test")        return unbuildable_reason::test;
    else if (r == "external")    return unbuildable_reason::external;
    else if (r == "unbuildable") return unbuildable_reason::unbuildable;
    else throw invalid_argument ("invalid unbuildable reason '" + r + '\'');
  }

  build_package_config*
  find (const string& name, build_package_configs& cs)
  {
    auto i (find_if (cs.begin (), cs.end (),
                     [&name] (const build_package_config& c)
                     {return c.name == name;}));

    return i != cs.end () ? &*i : nullptr;
  }
}
