#ifndef MOD_JWT_HXX
#define MOD_JWT_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/module-options.hxx>

#include <chrono>

namespace brep
{
  // Generate a JSON Web Token (JWT), defined in RFC 7519.
  //
  // A JWT is essentially the token issuer's name along with a number of
  // claims, signed with a private key.
  //
  // Note that only GitHub's requirements are implemented, not the entire JWT
  // spec; see the source file for details.
  //
  // The token expires when the validity period has elapsed.
  //
  // Return the token or empty if openssl exited with a non-zero status.
  //
  // Throw process_error or io_error (both derived from std::system_error) if
  // openssl could not be executed or communication with its process failed.
  //
  string
  gen_jwt (const options::openssl_options&,
           const path& private_key,
           const string& issuer,
           const std::chrono::minutes& validity_period);
}

#endif
