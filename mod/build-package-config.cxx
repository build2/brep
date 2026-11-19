// file      : mod/build-package-config.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/build-package-config.hxx>

namespace brep
{
  using namespace std;

  pair<string, string>
  split_package_build_config_name (const string& n)
  {
    size_t p (n.find ('+'));
    return p != string::npos
      ? make_pair (string (n, 0, p), string (n, p + 1))
      : make_pair (n, string ());
  }

  map<string, string>
  package_build_config_variants (const multimap<string, string>& variants)
  {
    map<string, string> r;

    for (const auto& v: variants)
    {
      auto i (r.find (v.first));

      if (i != r.end ())
      {
        i->second += '\n';
        i->second += v.second;
      }
      else
        r[v.first] = v.second;
    }

    // Make sure an entry for the base package configuration is always
    // present.
    //
    r.emplace ("", "");
    return r;
  }
}
