#ifndef MOD_HMAC_HXX
#define MOD_HMAC_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/module-options.hxx>

namespace brep
{
  // Compute the HMAC-SHA256 message authentication code over a message using
  // the given key (alpha-numeric string, not encoded).
  //
  // Return the HMAC value or throw std::system_error in case of an error.
  //
  // Example output:
  //
  //   5e822587094c68e646db8b916da1db2056d92f1dea4252136a533b4147a30cb7
  //
  // Note that although any cryptographic hash function can be used to compute
  // an HMAC, this implementation supports only SHA-256.
  //
  string
  compute_hmac (const options::openssl_options&,
                const void* message, size_t len,
                const char* key);
}

#endif
