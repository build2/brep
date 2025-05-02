#include <mod/hmac.hxx>

#include <libbutl/openssl.hxx>

using namespace std;
using namespace butl;

string brep::
compute_hmac (const options::openssl_options& o,
              const void* m, size_t l,
              const char* k)
{
  try
  {
    fdpipe errp (fdopen_pipe ()); // stderr pipe.

    // To compute an HMAC over stdin with the key <secret>:
    //
    //   openssl dgst -sha256 -hmac <secret>
    //
    // Note that since openssl 3.0 the `mac` command is the preferred method
    // for generating HMACs. For future reference, the equivalent command
    // would be:
    //
    //   openssl mac -digest SHA256 -macopt "key:<secret>" HMAC
    //
    // Note that for the above openssl openssl-dgst command the default output
    // format may differ for different versions of openssl. For example:
    //
    // 1.1.0: (stdin)= 499169db545f275b3a2daa89eb94ef727bd731256a79e90d797d221e2afe4858
    // 3.1.1: SHA2-256(stdin)= 499169db545f275b3a2daa89eb94ef727bd731256a79e90d797d221e2afe4858
    //
    // Thus, let's request the output in the coreutils format (-r option):
    //
    // 499169db545f275b3a2daa89eb94ef727bd731256a79e90d797d221e2afe4858 *stdin
    //
    // Also note that here we assume both output and diagnostics will fit into pipe
    // buffers and don't poll both with fdselect().
    //
    openssl os (path ("-"), // Read message from openssl::out.
                path ("-"), // Write output to openssl::in.
                process::pipe (errp.in.get (), move (errp.out)),
                process_env (o.openssl (), o.openssl_envvar ()),
                "dgst", o.openssl_option (),
                "-sha256",
                "-hmac", k,
                "-r");

    ifdstream err (move (errp.in));

    string h; // The HMAC value.
    try
    {
      // In case of an exception, skip and close input after output.
      //
      // Note: re-open in/out so that they get automatically closed on
      // an exception.
      //
      ifdstream in (os.in.release (), fdstream_mode::skip);
      ofdstream out (os.out.release ());

      // Write the message to openssl's input.
      //
      out.write (static_cast<const char*> (m), l);
      out.close ();

      // Read the HMAC value from openssl's output.
      //
      getline (in, h);
      in.close ();
    }
    catch (const io_error& e)
    {
      // If the process exits with non-zero status, assume the IO error is due
      // to that and fall through.
      //
      if (os.wait ())
      {
        throw_generic_error (
          e.code ().value (),
          (string ("unable to read/write openssl stdout/stdin: ") +
           e.what ()).c_str ());
      }
    }

    if (!os.wait ())
    {
      string et (err.read_text ());
      throw_generic_error (EINVAL,
                           ("non-zero openssl exit status: " + et).c_str ());
    }

    err.close ();

    // Verify the openssl output string and strip the ' *stdin' suffix.
    //
    if (h.find (' ') != 64)
      throw_generic_error (EINVAL, "unable to parse openssl stdout");

    h.resize (64);

    return h;
  }
  catch (const process_error& e)
  {
    throw_generic_error (
      e.code ().value (),
      (string ("unable to execute openssl: ") + e.what ()).c_str ());
  }
  catch (const io_error& e)
  {
    // Unable to read diagnostics from stderr.
    //
    throw_generic_error (
      e.code ().value (),
      (string ("unable to read openssl stderr : ") + e.what ()).c_str ());
  }
}
