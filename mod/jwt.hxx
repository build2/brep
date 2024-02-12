#ifndef MOD_JWT_HXX
#define MOD_JWT_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/module-options.hxx>

#include <chrono>

namespace brep
{
  // Generate a JSON Web Token (JWT), defined in RFC7519.
  //
  // A JWT is essentially the token issuer's name along with a number of
  // claims, signed with a private key.
  //
  // Note that only GitHub's requirements are implemented, not the entire JWT
  // spec; see the source file for details.
  //
  // The token expires when the validity period has elapsed.
  //
  // The backdate argument specifies the number of seconds to subtract from
  // the "issued at" time in order to combat potential clock drift (which can
  // casue the token to be not valid yet).
  //
  // Return the token or std::system_error in case if an error.
  //
  string
  gen_jwt (const options::openssl_options&,
           const path& private_key,
           const string& issuer,
           const std::chrono::minutes& validity_period,
           const std::chrono::seconds& backdate = 60);
}

#endif
