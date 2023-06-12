// file      : mod/build-result-module.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_BUILD_RESULT_MODULE_HXX
#define MOD_BUILD_RESULT_MODULE_HXX

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <libbrep/build.hxx>

#include <mod/module-options.hxx>
#include <mod/database-module.hxx>
#include <mod/build-config-module.hxx>

namespace brep
{
  // Base class for modules that handle the build task results.
  //
  // Specifically, it loads build controller configuration, initializes the
  // build database instance, and provides utilities for parsing and
  // authenticating the build task session.
  //
  class build_result_module: public database_module,
                             protected build_config_module
  {
  protected:
    build_result_module () = default;

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    explicit
    build_result_module (const build_result_module&);

    void
    init (const options::build&, const options::build_db&);

    using handler::init; // Unhide.

    // Parse the build task session and verify that the session matches the
    // tenant. Throw invalid_argument on errors.
    //
    struct parse_session_result
    {
      build_id id;
      brep::version package_version;
      brep::version toolchain_version;
      brep::timestamp timestamp;
    };

    parse_session_result
    parse_session (const string&) const;

    // Return true if bbot agent authentication is disabled or the agent is
    // recognized and challenge matches. If the session authentication fails
    // (challenge is not expected, expected but doesn't match, etc), then log
    // the failure reason with the warning severity and return false.
    //
    // Note that the session argument is used only for logging.
    //
    bool
    authenticate_session (const options::build&,
                          const optional<vector<char>>& challenge,
                          const build&,
                          const string& session) const;

  protected:
    // True if the openssl version is greater or equal to 3.0.0 and so pkeyutl
    // needs to be used instead of rsautl.
    //
    // Note that openssl 3.0.0 deprecates rsautl in favor of pkeyutl.
    //
    bool use_openssl_pkeyutl_;
  };
}

#endif // MOD_BUILD_RESULT_MODULE_HXX
