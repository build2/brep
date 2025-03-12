// file      : mod/mod-ci-github-post.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_CI_GITHUB_POST_HXX
#define MOD_MOD_CI_GITHUB_POST_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <libbutl/curl.hxx>

namespace brep
{
  // GitHub response header name and value. The value is absent if the
  // header is not present.
  //
  struct github_response_header
  {
    string           name;
    optional<string> value;
  };

  using github_response_headers = vector<github_response_header>;

  // Send a POST request to the GitHub API endpoint `ep`, parse GitHub's JSON
  // response into `rs` (only for 200 codes), and return the HTTP status code.
  //
  // The endpoint `ep` should not have a leading slash.
  //
  // Pass additional HTTP headers in `hdrs`. For example:
  //
  //   "HeaderName: header value"
  //
  // To retrieve response headers, specify their names in `rsp_hdrs` and the
  // received header value will be saved in the corresponding pair's
  // second. Skip/ignore response headers if rsp_hdrs is null or empty. Note
  // that currently only single-line headers are supported.
  //
  // Throw invalid_argument if unable to parse the response headers,
  // invalid_json_input (derived from invalid_argument) if unable to parse the
  // response body, and system_error in other cases.
  //
  template <typename T>
  uint16_t
  github_post (T& rs,
               const string& ep,
               const strings& hdrs,
               const string& body = "",
               github_response_headers* rsp_hdrs = nullptr)
  {
    using namespace butl;

    // Convert the header values to curl header option/value pairs.
    //
    strings hdr_opts;

    for (const string& h: hdrs)
    {
      hdr_opts.push_back ("--header");
      hdr_opts.push_back (h);
    }

    // Run curl.
    //
    try
    {
      // Pass --include to print the HTTP status line (followed by the response
      // headers) so that we can get the response status code.
      //
      // Suppress the --fail option which causes curl to exit with status 22
      // in case of an error HTTP response status code (>= 400) otherwise we
      // can't get the status code.
      //
      // Note that butl::curl also adds --location to make curl follow redirects
      // (which is recommended by GitHub).
      //
      // The API version `2022-11-28` is the only one currently supported. If
      // the X-GitHub-Api-Version header is not passed this version will be
      // chosen by default.
      //
      fdpipe errp (fdopen_pipe ()); // stderr pipe.

      curl c (path ("-"), // Read input from curl::out.
              path ("-"), // Write response to curl::in.
              process::pipe (errp.in.get (), move (errp.out)),
              curl::post,
              curl::flags::no_fail,
              "https://api.github.com/" + ep,
              "--no-fail", // Don't fail if response status code >= 400.
              "--include", // Output response headers for status code.
              "--header", "Accept: application/vnd.github+json",
              "--header", "X-GitHub-Api-Version: 2022-11-28",
              move (hdr_opts));

      ifdstream err (move (errp.in));

      // Parse the HTTP response.
      //
      uint16_t sc; // Status code.
      try
      {
        // Note: re-open in/out so that they get automatically closed on
        // exception.
        //
        ifdstream in (c.in.release (), fdstream_mode::skip);
        ofdstream out (c.out.release ());

        // Write request body to out.
        //
        if (!body.empty ())
          out << body;
        out.close ();

        // True if no response headers were requested to be saved.
        //
        bool skip_headers (rsp_hdrs == nullptr || rsp_hdrs->empty ());

        // Read the response status code. May throw invalid_argument.
        //
        sc = curl::read_http_status (in, skip_headers).code;

        // Read the response headers and save them in rsp_hdrs if requested.
        //
        if (!skip_headers)
        {
          // Note: the following code is based on bdep/http-service.cxx.

          // Check if the line contains the specified header and return its
          // value if that's the case. Return nullopt otherwise.
          //
          // Note that we don't expect the header values that we are
          // interested in to span over multiple lines.
          //
          string l;
          auto header = [&l] (const string& name) -> optional<string>
          {
            size_t n (name.size ());
            if (!(icasecmp (name, l, n) == 0 && l[n] == ':'))
              return nullopt;

            string r;
            size_t p (l.find_first_not_of (' ', n + 1)); // The value begin.
            if (p != string::npos)
            {
              size_t e (l.find_last_not_of (' '));       // The value end.
              assert (e != string::npos && e >= p);

              r = string (l, p, e - p + 1);
            }

            return optional<string> (move (r));
          };

          // Read response headers and save the requested ones.
          //
          for (size_t saved_count (0); // Number of headers saved.
               !(l = curl::read_http_response_line (in)).empty (); )
          {
            // Note that we have to finish reading all the headers so cannot
            // bail out.
            //
            if (saved_count != rsp_hdrs->size ())
            {
              for (github_response_header& rh: *rsp_hdrs)
              {
                if (optional<string> v = header (rh.first))
                {
                  rh.second = move (v);
                  ++saved_count;
                  break;
                }
              }
            }
          }
        }

        // Parse the response body if the status code is in the 200 range.
        //
        if (sc >= 200 && sc < 300)
        {
          // Use endpoint name as input name (useful to have it propagated
          // in exceptions).
          //
          json::parser p (in, ep /* name */);
          rs = T (p);
        }

        in.close ();
      }
      catch (const io_error& e)
      {
        // If the process exits with non-zero status, assume the IO error is due
        // to that and fall through.
        //
        if (c.wait ())
        {
          throw_generic_error (
            e.code ().value (),
            (string ("unable to read curl stdout: ") + e.what ()).c_str ());
        }
      }
      catch (const json::invalid_json_input&)
      {
        // If the process exits with non-zero status, assume the JSON error is
        // due to that and fall through.
        //
        if (c.wait ())
          throw;
      }

      if (!c.wait ())
      {
        string et (err.read_text ());
        throw_generic_error (EINVAL,
                             ("non-zero curl exit status: " + et).c_str ());
      }

      err.close ();

      return sc;
    }
    catch (const process_error& e)
    {
      throw_generic_error (
        e.code ().value (),
        (string ("unable to execute curl:") + e.what ()).c_str ());
    }
    catch (const io_error& e)
    {
      // Unable to read diagnostics from stderr.
      //
      throw_generic_error (
        e.code ().value (),
        (string ("unable to read curl stderr : ") + e.what ()).c_str ());
    }
  }
}

#endif // MOD_MOD_CI_GITHUB_POST_HXX
