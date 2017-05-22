// file      : clean/types-parsers.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <chrono>
#include <string> // strtoull()

#include <clean/types-parsers.hxx>

#include <clean/options-types.hxx>
#include <clean/clean-options.hxx> // cli namespace

using namespace std;
using namespace brep;

namespace cli
{
  void parser<toolchain_timeouts>::
  parse (toolchain_timeouts& x, bool& xs, scanner& s)
  {
    const char* o (s.next ());

    if (!s.more ())
      throw missing_value (o);

    string ov (s.next ());
    size_t p (ov.find ('='));

    timestamp now (timestamp::clock::now ());

    // Convert timeout duration into the time point.
    //
    auto timeout = [o, &ov, &now] (const string& tm) -> timestamp
    {
      char* e (nullptr);
      uint64_t t (strtoull (tm.c_str (), &e, 10));

      if (*e != '\0' || tm.empty ())
        throw invalid_value (o, ov);

      if (t == 0)
        return timestamp_nonexistent;

      return now - chrono::duration<uint64_t, ratio<86400>> (t);
    };

    if (p == string::npos)
      x[string ()] = timeout (ov); // Default timeout.
    else
    {
      string k (ov, 0, p);
      if (k.empty ())
        throw invalid_value (o, ov);

      x[k] = timeout (string (ov, p + 1));
    }

    xs = true;
  }
}
