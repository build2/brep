// file      : mod/utility.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/utility.hxx>

#include <chrono>
#include <random>
#include <thread> // this_thread::sleep_for(), this_thread::yield()

#include <libbutl/path-pattern.hxx>

using namespace std;

static thread_local mt19937 rand_gen (random_device {} ());

namespace brep
{
  string
  wildcard_to_similar_to_pattern (const string& wildcard)
  {
    using namespace butl;

    if (wildcard.empty ())
      return "%";

    string r;
    for (const path_pattern_term& pt: path_pattern_iterator (wildcard))
    {
      switch (pt.type)
      {
      case path_pattern_term_type::question: r += '_'; break;
      case path_pattern_term_type::star:     r += '%'; break;
      case path_pattern_term_type::bracket:
        {
          // Copy the bracket expression translating the inverse character, if
          // present.
          //
          size_t n (r.size ());
          r.append (pt.begin, pt.end);

          if (r[n + 1] == '!') // ...[!... ?
            r[n + 1] = '^';

          break;
        }
      case path_pattern_term_type::literal:
        {
          char c (get_literal (pt));

          // Escape the special characters.
          //
          // Note that '.' is not a special character for SIMILAR TO.
          //
          switch (c)
          {
          case '\\':
          case '%':
          case '_':
          case '|':
          case '+':
          case '{':
          case '}':
          case '(':
          case ')':
          case '[':
          case ']': r += '\\'; break;
          }

          r += c;
          break;
        }
      }
    }

    return r;
  }

  void
  sleep_before_retry (size_t retry, size_t max_time)
  {
    if (retry != 0)
    {
      size_t ms (
        uniform_int_distribution<unsigned long> (
          1, static_cast<unsigned long> (max_time)) (rand_gen));

      this_thread::sleep_for (chrono::milliseconds (ms));
    }
    else
      this_thread::yield ();
  }
}
