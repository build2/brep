#include <mod/jwt.hxx>

#include <libbutl/base64.hxx>
#include <libbutl/openssl.hxx>
#include <libbutl/timestamp.hxx>
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
         const std::chrono::minutes& vp)
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
    seconds iat (
        duration_cast<seconds> (system_clock::now ().time_since_epoch ()));

    // Expiration time.
    //
    seconds exp (iat + vp);

    vector<char> b;
    json::buffer_serializer s (b);

    s.begin_object ();
    s.member ("iss", iss);
    s.member ("iat", iat.count ());
    s.member ("exp", exp.count ());
    s.end_object ();

    p = base64url_encode (b);
  }

  // Create the signature.
  //

  // The signature (base64url-encoded). Will be left empty if openssl exits
  // with a non-zero status. @@
  //
  string s;
  try
  {
    // Sign the concatenated header and payload using openssl.
    //
    //   openssl dgst -sha256 -sign <pkey> file...
    //
    // Note that RSA is indicated by the contents of the private key.
    //
    // Note that here we assume both output and diagnostics will fit into pipe
    // buffers and don't both with fdselect().
    //
    openssl os (path ("-"), // Read message from openssl::out.
                path ("-"), // Write output to openssl::in.
                2,          // Diagnostics to stderr.
                process_env (o.openssl (), o.openssl_envvar ()),
                "dgst", o.openssl_option (), "-sha256", "-sign", pk);

    // @@ TODO redirect stderr to pipe/ofdstream.

    try
    {
      // Write the concatenated header and payload to openssl's input.
      //
      os.out << h << '.' << p;
      os.out.close ();

      // Read the binary signature from openssl's output.
      //
      vector<char> bs (os.in.read_binary ());
      os.in.close ();
    }
    catch (const io_error&)
    {
      if (!os.wait ())
        throw system_error (); // @@

      // Fall through.
    }

    if (!os.wait ())
      throw system_error (); // @@

    s = base64url_encode (bs);
  }
  catch ()
  {
    // @@ TODO: catch all possible errors and translate to suitable
    //    system_error.
  }

  // Return the token, or empty if openssl exited with a non-zero status. @@
  //
  return !s.empty ()
         ? h + '.' + p + '.' + s
         : "";
}
