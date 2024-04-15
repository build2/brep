// file      : mod/build-result-module.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/build-result-module.hxx>

#include <odb/database.hxx>

#include <libbutl/openssl.hxx>
#include <libbutl/fdstream.hxx>
#include <libbutl/process-io.hxx>
#include <libbutl/semantic-version.hxx>

#include <libbrep/build-package.hxx>
#include <libbrep/build-package-odb.hxx>

namespace brep
{
  using namespace std;
  using namespace butl;

  // While currently the user-defined copy constructor is not required (we
  // don't need to deep copy nullptr's), it is a good idea to keep the
  // placeholder ready for less trivial cases.
  //
  build_result_module::
  build_result_module (const build_result_module& r)
      : database_module (r),
        build_config_module (r),
        use_openssl_pkeyutl_ (r.initialized_ ? r.use_openssl_pkeyutl_ : false)
  {
  }

  void build_result_module::
  init (const options::build& bo, const options::build_db& bdo)
  {
    HANDLER_DIAG;

    build_config_module::init (bo);
    database_module::init (bdo, bdo.build_db_retry ());

    try
    {
      optional<openssl_info> oi (
        openssl::info ([&trace, this] (const char* args[], size_t n)
                       {
                         l2 ([&]{trace << process_args {args, n};});
                       },
                       2,
                       bo.openssl ()));

      use_openssl_pkeyutl_ = oi                    &&
                             oi->name == "OpenSSL" &&
                             oi->version >= semantic_version {3, 0, 0};
    }
    catch (const system_error& e)
    {
      fail << "unable to obtain openssl version: " << e;
    }
  }

  build_result_module::parse_session_result build_result_module::
  parse_session (const string& s) const
  {
    using brep::version; // Not to confuse with module::version.

    parse_session_result r;

    size_t p (s.find ('/')); // End of tenant.

    if (p == string::npos)
      throw invalid_argument ("no package name");

    if (tenant.compare (0, tenant.size (), s, 0, p) != 0)
      throw invalid_argument ("tenant mismatch");

    size_t b (p + 1);    // Start of package name.
    p = s.find ('/', b); // End of package name.

    if (p == b)
      throw invalid_argument ("empty package name");

    if (p == string::npos)
      throw invalid_argument ("no package version");

    package_name name;

    try
    {
      name = package_name (string (s, b, p - b));
    }
    catch (const invalid_argument& e)
    {
      throw invalid_argument (
        string ("invalid package name : ") + e.what ());
    }

    b = p + 1;           // Start of version.
    p = s.find ('/', b); // End of version.

    if (p == string::npos)
      throw invalid_argument ("no target");

    auto parse_version = [&s, &b, &p] (const char* what) -> version
    {
      // Intercept exception handling to add the parsing error attribution.
      //
      try
      {
        return brep::version (string (s, b, p - b));
      }
      catch (const invalid_argument& e)
      {
        throw invalid_argument (
          string ("invalid ") + what + ": " + e.what ());
      }
    };

    r.package_version = parse_version ("package version");

    b = p + 1;           // Start of target.
    p = s.find ('/', b); // End of target.

    if (p == string::npos)
      throw invalid_argument ("no target configuration name");

    target_triplet target;
    try
    {
      target = target_triplet (string (s, b, p - b));
    }
    catch (const invalid_argument& e)
    {
      throw invalid_argument (string ("invalid target: ") + e.what ());
    }

    b = p + 1;           // Start of target configuration name.
    p = s.find ('/', b); // End of target configuration name.

    if (p == string::npos)
      throw invalid_argument ("no package configuration name");

    string target_config (s, b, p - b);

    if (target_config.empty ())
      throw invalid_argument ("empty target configuration name");

    b = p + 1;           // Start of package configuration name.
    p = s.find ('/', b); // End of package configuration name.

    if (p == string::npos)
      throw invalid_argument ("no toolchain name");

    string package_config (s, b, p - b);

    if (package_config.empty ())
      throw invalid_argument ("empty package configuration name");

    b = p + 1;           // Start of toolchain name.
    p = s.find ('/', b); // End of toolchain name.

    if (p == string::npos)
      throw invalid_argument ("no toolchain version");

    string toolchain_name (s, b, p - b);

    if (toolchain_name.empty ())
      throw invalid_argument ("empty toolchain name");

    b = p + 1;           // Start of toolchain version.
    p = s.find ('/', b); // End of toolchain version.

    if (p == string::npos)
      throw invalid_argument ("no timestamp");

    r.toolchain_version = parse_version ("toolchain version");

    r.id = build_id (package_id (move (tenant), move (name), r.package_version),
                     move (target),
                     move (target_config),
                     move (package_config),
                     move (toolchain_name),
                     r.toolchain_version);

    try
    {
      size_t tsn;
      string ts (s, p + 1);

      r.timestamp = timestamp (chrono::duration_cast<timestamp::duration> (
                                 chrono::nanoseconds (stoull (ts, &tsn))));

      if (tsn != ts.size ())
        throw invalid_argument ("trailing junk");
    }
    // Handle invalid_argument or out_of_range (both derive from logic_error),
    // that can be thrown by stoull().
    //
    catch (const logic_error& e)
    {
      throw invalid_argument (string ("invalid timestamp: ") + e.what ());
    }

    return r;
  }

  bool build_result_module::
  authenticate_session (const options::build& o,
                        const optional<vector<char>>& challenge,
                        const build& b,
                        const string& session) const
  {
    HANDLER_DIAG;

    auto warn_auth = [&session, &warn] (const string& d)
    {
      warn << "session '" << session << "' authentication failed: " << d;
    };

    bool r (false);

    // Must both be present or absent.
    //
    if (!b.agent_challenge != !challenge)
    {
      warn_auth (challenge ? "unexpected challenge": "challenge is expected");
    }
    else if (bot_agent_key_map_ == nullptr) // Authentication is disabled.
    {
      r = true;
    }
    else if (!b.agent_challenge) // Authentication is recently enabled.
    {
      warn_auth ("challenge is required now");
    }
    else
    {
      assert (b.agent_fingerprint && challenge);

      auto auth = [&challenge,
                   &b,
                   &o,
                   &fail, &trace,
                   &warn_auth,
                   this] (const path& key)
      {
        bool r (false);

        try
        {
          openssl os ([&trace, this] (const char* args[], size_t n)
                      {
                        l2 ([&]{trace << process_args {args, n};});
                      },
                      path ("-"), fdstream_mode::text, 2,
                      process_env (o.openssl (), o.openssl_envvar ()),
                      use_openssl_pkeyutl_ ? "pkeyutl" : "rsautl",
                      o.openssl_option (),
                      use_openssl_pkeyutl_ ? "-verifyrecover" : "-verify",
                      "-pubin",
                      "-inkey", key);

          for (const auto& c: *challenge)
            os.out.put (c); // Sets badbit on failure.

          os.out.close ();

          string s;
          getline (os.in, s);

          bool v (os.in.eof ());
          os.in.close ();

          if (os.wait () && v)
          {
            r = (s == *b.agent_challenge);

            if (!r)
              warn_auth ("challenge mismatched");
          }
          else // The signature is presumably meaningless.
            warn_auth ("unable to verify challenge");
        }
        catch (const system_error& e)
        {
          fail << "unable to verify challenge: " << e;
        }

        return r;
      };

      const string& fp (*b.agent_fingerprint);
      auto i (bot_agent_key_map_->find (fp));

      // Note that it is possible that the default vs custom bot
      // classification has changed since the task request time. It feels that
      // there is nothing wrong with that and we will handle that
      // automatically.
      //
      if (i != bot_agent_key_map_->end ()) // Default bot?
      {
        r = auth (i->second);
      }
      else                                 // Custom bot.
      {
        shared_ptr<build_public_key> k (
          build_db_->find<build_public_key> (public_key_id (b.tenant, fp)));

        if (k != nullptr)
        {
          // Temporarily save the key data to disk (note that it's the
          // challenge which is passed via stdin to openssl). Hopefully /tmp
          // is using tmpfs.
          //
          auto_rmfile arm;

          try
          {
            arm = auto_rmfile (path::temp_path ("brep-custom-bot-key"));
          }
          catch (const system_error& e)
          {
            fail << "unable to obtain temporary file: " << e;
          }

          try
          {
            ofdstream os (arm.path);
            os << *k;
            os.close ();
          }
          catch (const io_error& e)
          {
            fail << "unable to write to '" << arm.path << "': " << e;
          }

          r = auth (arm.path);
        }
        else
        {
          // The agent's key is recently replaced.
          //
          warn_auth ("agent's public key not found");
        }
      }
    }

    return r;
  }
}
