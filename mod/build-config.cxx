// file      : mod/build-config.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/build-config>

#include <map>

namespace brep
{
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
}
