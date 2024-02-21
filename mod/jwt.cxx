#include <mod/jwt.hxx>

#include <libbutl/base64.hxx>
#include <libbutl/openssl.hxx>
#include <libbutl/json/serializer.hxx>

using namespace std;
using namespace butl;

// Note that only GitHub's requirements are implemented, not the entire JWT
// spec. The following elements are currently supported:
//
// - The RS256 message authentication code algorithm (RSA with SHA256)
// - The `typ` and `alg` header fields
// - The `iat`, `exp`, and `iss` claims
//
// A JWT consists of a message and its signature.
//
// The message consists of a base64url-encoded JSON header and payload (set of
// claims). The signature is calculated over the message and then also
// base64url-encoded.
//
// base64url is base64 with a slightly different alphabet and optional padding
// to make it URL and filesystem safe. See base64.hxx for details.
//
// Header:
//
// {
//   "typ": "JWT",
//   "alg": "RS256"
// }
//
// Payload:
//
// {
//   "iat": 1234567,
//   "exp": 1234577,
//   "iss": "MyName"
// }
//
// Where:
// iat := Issued At (NumericDate: seconds since 1970-01-01T00:00:00Z UTC)
// exp := Expiration Time (NumericDate)
// iss := Issuer
//
// Signature:
//
//   RSA_SHA256(PKEY, base64url($header) + '.' + base64url($payload))
//
// JWT:
//
//   base64url($header) + '.' + base64url($payload) + '.' + base64url($signature)
//
string brep::
gen_jwt (const options::openssl_options& o,
         const path& pk,
         const string& iss,
         const chrono::seconds& vp,
         const chrono::seconds& bd)
{
  // Create the header.
  //
  string h; // Header (base64url-encoded).
  {
    vector<char> b;
    json::buffer_serializer s (b, 0 /* indentation */);

    s.begin_object ();
    s.member ("typ", "JWT");
    s.member ("alg", "RS256"); // RSA with SHA256.
    s.end_object ();

    h = base64url_encode (b);
  }

  // Create the payload.
  //
  string p; // Payload (base64url-encoded).
  {
    using namespace std::chrono;

    // "Issued at" time.
    //
    seconds iat (duration_cast<seconds> (
        system_clock::now ().time_since_epoch () - bd));

    // Expiration time.
    //
    seconds exp (iat + vp);

    vector<char> b;
    json::buffer_serializer s (b, 0 /* indentation */);

    s.begin_object ();
    s.member ("iss", iss);
    s.member ("iat", iat.count ());
    s.member ("exp", exp.count ());
    s.end_object ();

    p = base64url_encode (b);
  }

  // Create the signature.
  //
  string s; // Signature (base64url-encoded).
  try
  {
    // Sign the concatenated header and payload using openssl.
    //
    //   openssl dgst -sha256 -sign <pkey> file...
    //
    // Note that RSA is indicated by the contents of the private key.
    //
    // Note that here we assume both output and diagnostics will fit into pipe
    // buffers and don't poll both with fdselect().
    //
    fdpipe errp (fdopen_pipe ()); // stderr pipe.

    openssl os (path ("-"), // Read message from openssl::out.
                path ("-"), // Write output to openssl::in.
                process::pipe (errp.in.get (), move (errp.out)),
                process_env (o.openssl (), o.openssl_envvar ()),
                "dgst", o.openssl_option (), "-sha256", "-sign", pk);

    ifdstream err (move (errp.in));

    vector<char> bs; // Binary signature (openssl output).
    try
    {
      // In case of exception, skip and close input after output.
      //
      // Note: re-open in/out so that they get automatically closed on
      // exception.
      //
      ifdstream in (os.in.release (), fdstream_mode::skip);
      ofdstream out (os.out.release ());

      // Write the concatenated header and payload to openssl's input.
      //
      out << h << '.' << p;
      out.close ();

      // Read the binary signature from openssl's output.
      //
      bs = in.read_binary ();
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

    s = base64url_encode (bs);
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

  return h + '.' + p + '.' + s; // Return the token.
}
