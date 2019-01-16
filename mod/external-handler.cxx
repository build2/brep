// file      : mod/external-handler.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/external-handler.hxx>

#include <sys/time.h>   // timeval
#include <sys/select.h>

#include <ratio>        // ratio_greater_equal
#include <chrono>
#include <sstream>
#include <cstdlib>      // strtoul()
#include <type_traits>  // static_assert
#include <system_error> // error_code, generic_category()

#include <libbutl/process.mxx>
#include <libbutl/fdstream.mxx>
#include <libbutl/process-io.mxx> // operator<<(ostream, process_args)

using namespace std;
using namespace butl;

namespace brep
{
  namespace external_handler
  {
    optional<result_manifest>
    run (const path& handler,
         const strings& args,
         const dir_path& data_dir,
         size_t tm,
         const basic_mark& error,
         const basic_mark& warn,
         const basic_mark* trace)
    {
      using parser  = manifest_parser;
      using parsing = manifest_parsing;

      using namespace chrono;

      using time_point = system_clock::time_point;
      using duration   = system_clock::duration;

      // Make sure that the system clock has at least milliseconds resolution.
      //
      static_assert(
        ratio_greater_equal<milliseconds::period, duration::period>::value,
        "The system clock resolution is too low");

      // For the sake of the documentation we will call the handler's normal
      // exit with 0 code "successful termination".
      //
      // To make sure the handler process execution doesn't exceed the
      // specified timeout we set the non-blocking mode for the process
      // stdout-reading stream, try to read from it with the 10 milliseconds
      // timeout and check the process execution time between the reads. We
      // then kill the process if the execution time is exceeded.
      //
      optional<milliseconds> timeout;

      if (tm != 0)
        timeout = milliseconds (tm * 1000);

      // Note that due to the non-blocking mode we cannot just pass the stream
      // to the manifest parser constructor. So we buffer the data in the
      // string stream and then parse that.
      //
      stringstream ss;

      assert (!data_dir.empty ());

      // Normally the data directory leaf component identifies the entity
      // being handled. We will use it as a reference for logging.
      //
      string ref (data_dir.leaf ().string ());

      for (;;) // Breakout loop.
        try
        {
          fdpipe pipe (fdopen_pipe ()); // Can throw io_error.

          // Redirect the diagnostics to the web server error log.
          //
          process pr (
            process_start_callback ([&trace] (const char* args[], size_t n)
                                    {
                                      if (trace != nullptr)
                                        *trace << process_args {args, n};
                                    },
                                    0     /* stdin  */,
                                    pipe  /* stdout */,
                                    2     /* stderr */,
                                    handler,
                                    args,
                                    data_dir));
          pipe.out.close ();

          auto kill = [&pr, &warn, &handler, &ref] ()
            {
              // We may still end up well (see below), thus this is a warning.
              //
              warn << "ref " << ref << ": process " << handler
              << " execution timeout expired";

              pr.kill ();
            };

          try
          {
            ifdstream is (move (pipe.in), fdstream_mode::non_blocking);

            const size_t nbuf (8192);
            char buf[nbuf];

            while (is.is_open ())
            {
              time_point start;
              milliseconds wd (10); // Max time to wait for the data portion.

              if (timeout)
              {
                start = system_clock::now ();

                if (*timeout < wd)
                  wd = *timeout;
              }

              timeval tm {wd.count () / 1000        /* seconds */,
                          wd.count () % 1000 * 1000 /* microseconds */};

              fd_set rd;
              FD_ZERO (&rd);
              FD_SET  (is.fd (), &rd);

              int r (select (is.fd () + 1, &rd, nullptr, nullptr, &tm));

              if (r == -1)
              {
                // Don't fail if the select() call was interrupted by the
                // signal.
                //
                if (errno != EINTR)
                  throw_system_ios_failure (errno, "select failed");
              }
              else if (r != 0) // Is data available?
              {
                assert (FD_ISSET (is.fd (), &rd));

                // The only leagal way to read from non-blocking ifdstream.
                //
                streamsize n (is.readsome (buf, nbuf));

                // Close the stream (and bail out) if the end of the data is
                // reached. Otherwise cache the read data.
                //
                if (is.eof ())
                  is.close ();
                else
                {
                  // The data must be available.
                  //
                  // Note that we could keep reading until the readsome() call
                  // returns 0. However, this way we could potentially exceed
                  // the timeout significantly for some broken handler that
                  // floods us with data. So instead, we will be checking the
                  // process execution time after every data chunk read.
                  //
                  assert (n != 0);

                  ss.write (buf, n);
                }
              }
              else // Timeout occured.
              {
                // Normally, we don't expect timeout to occur on the pipe read
                // operation if the process has terminated successfully, as
                // all its output must already be buffered (including eof).
                // However, there can be some still running handler's child
                // that has inherited the parent's stdout. In this case we
                // assume that we have read all the handler's output, close
                // the stream, log the warning and bail out.
                //
                if (pr.exit)
                {
                  // We keep reading only upon successful handler termination.
                  //
                  assert (*pr.exit);

                  is.close ();

                  warn << "ref " << ref << ": process " << handler
                       << " stdout is not closed after termination (possibly "
                       << "handler's child still running)";
                }
              }

              if (timeout)
              {
                time_point now (system_clock::now ());

                // Assume we have waited the full amount if the time
                // adjustment is detected.
                //
                duration d (now > start ? now - start : wd);

                // If the timeout is not fully exhausted, then decrement it and
                // try to read some more data from the handler' stdout.
                // Otherwise, kill the process, if not done yet.
                //
                // Note that it may happen that we are killing an already
                // terminated process, in which case kill() just sets the
                // process exit information. On the other hand it's guaranteed
                // that the process is terminated after the kill() call, and
                // so the pipe is presumably closed on the write end (see
                // above for details). Thus, if the process terminated
                // successfully, we will continue reading until eof is
                // reached or read timeout occurred. Yes, it may happen that
                // we will succeed even with the kill.
                //
                if (*timeout > d)
                  *timeout -= duration_cast<milliseconds> (d);
                else if (!pr.exit)
                {
                  kill ();

                  assert (pr.exit);

                  // Close the stream (and bail out) if the process hasn't
                  // terminate successfully.
                  //
                  if (!*pr.exit)
                    is.close ();

                  *timeout = milliseconds::zero ();
                }
              }
            }

            assert (!is.is_open ());

            if (!timeout)
              pr.wait ();

            // If the process is not terminated yet, then wait for its
            // termination for the remaining time. Kill it if the timeout has
            // been exceeded and the process still hasn't terminate.
            //
            else if (!pr.exit && !pr.timed_wait (*timeout))
              kill ();

            assert (pr.exit); // The process must finally be terminated.

            if (*pr.exit)
              break; // Get out of the breakout loop.

            error << "ref " << ref << ": process " << handler << " "
                  << *pr.exit;

            // Fall through.
          }
          catch (const io_error& e)
          {
            if (pr.wait ())
              error << "ref " << ref << ": unable to read handler's output: "
                    << e;

            // Fall through.
          }

          return nullopt;
        }
      // Handle process_error and io_error (both derive from system_error).
      //
        catch (const system_error& e)
        {
          error << "ref " << ref << ": unable to execute '" << handler
                << "': " << e;

          return nullopt;
        }

      result_manifest r;

      // Parse and verify the manifest.
      //
      try
      {
        parser p (ss, handler.leaf ().string ());
        manifest_name_value nv (p.next ());

        auto bad_value ([&p, &nv] (const string& d) {
            throw parsing (p.name (), nv.value_line, nv.value_column, d);});

        if (nv.empty ())
          bad_value ("empty manifest");

        const string& n (nv.name);
        const string& v (nv.value);

        // The format version pair is verified by the parser.
        //
        assert (n.empty () && v == "1");

        // Save the format version pair.
        //
        r.values.push_back (move (nv));

        // Get and verify the HTTP status.
        //
        nv = p.next ();
        if (n != "status")
          bad_value ("no status specified");

        char* e (nullptr);
        unsigned long c (strtoul (v.c_str (), &e, 10)); // Can't throw.

        assert (e != nullptr);

        if (!(*e == '\0' && c >= 100 && c < 600))
          bad_value ("invalid HTTP status '" + v + "'");

        // Save the HTTP status.
        //
        r.status = static_cast<uint16_t> (c);
        r.values.push_back (move (nv));

        // Save the remaining name/value pairs.
        //
        for (nv = p.next (); !nv.empty (); nv = p.next ())
          r.values.push_back (move (nv));

        // Save end of manifest.
        //
        r.values.push_back (move (nv));
      }
      catch (const parsing& e)
      {
        error << "ref " << ref << ": unable to parse handler's output: " << e;
        return nullopt;
      }

      return optional<result_manifest> (move (r));
    }
  }
}
