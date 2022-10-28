// file      : mod/build-config-module.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/build-config-module.hxx>

#include <errno.h> // EIO

#include <map>
#include <sstream>

#include <libbutl/sha256.hxx>
#include <libbutl/utility.hxx>      // throw_generic_error()
#include <libbutl/openssl.hxx>
#include <libbutl/filesystem.hxx>   // dir_iterator, dir_entry

namespace brep
{
  using namespace std;
  using namespace butl;
  using namespace bpkg;

  // Return pointer to the shared build target configurations instance,
  // creating one on the first call. Throw tab_parsing on parsing error,
  // io_error on the underlying OS error. Note: not thread-safe.
  //
  static shared_ptr<const build_target_configs>
  shared_build_config (const path& p)
  {
    static map<path, weak_ptr<build_target_configs>> configs;

    auto i (configs.find (p));
    if (i != configs.end ())
    {
      if (shared_ptr<build_target_configs> c = i->second.lock ())
        return c;
    }

    shared_ptr<build_target_configs> c (
      make_shared<build_target_configs> (bbot::parse_buildtab (p)));

    configs[p] = c;
    return c;
  }

  // Return pointer to the shared build bot agent public keys map, creating
  // one on the first call. Throw system_error on the underlying openssl or OS
  // error. Note: not thread-safe.
  //
  using bot_agent_key_map = map<string, path>;

  static shared_ptr<const bot_agent_key_map>
  shared_bot_agent_keys (const options::openssl_options& o, const dir_path& d)
  {
    static map<dir_path, weak_ptr<bot_agent_key_map>> keys;

    auto i (keys.find (d));
    if (i != keys.end ())
    {
      if (shared_ptr<bot_agent_key_map> k = i->second.lock ())
        return k;
    }

    shared_ptr<bot_agent_key_map> ak (make_shared<bot_agent_key_map> ());

    // Intercept exception handling to make error descriptions more
    // informative.
    //
    // Path of the key being converted. Used for diagnostics.
    //
    path p;

    try
    {
      for (const dir_entry& de: dir_iterator (d, false /* ignore_dangling */))
      {
        if (de.path ().extension () == "pem" &&
            de.type () == entry_type::regular)
        {
          p = d / de.path ();

          openssl os (p, path ("-"), 2,
                      process_env (o.openssl (), o.openssl_envvar ()),
                      "pkey",
                      o.openssl_option (), "-pubin", "-outform", "DER");

          string fp (sha256 (os.in).string ());
          os.in.close ();

          if (!os.wait ())
            throw io_error ("");

          ak->emplace (move (fp), move (p));
        }
      }
    }
    catch (const io_error&)
    {
      ostringstream os;
      os << "unable to convert bbot agent pubkey " << p;
      throw_generic_error (EIO, os.str ().c_str ());
    }
    catch (const process_error& e)
    {
      ostringstream os;
      os << "unable to convert bbot agent pubkey " << p;
      throw_generic_error (e.code ().value (), os.str ().c_str ());
    }
    catch (const system_error& e)
    {
      ostringstream os;
      os<< "unable to iterate over agents keys directory '" << d << "'";
      throw_generic_error (e.code ().value (), os.str ().c_str ());
    }

    keys[d] = ak;
    return ak;
  }

  void build_config_module::
  init (const options::build& bo)
  {
    try
    {
      target_conf_ = shared_build_config (bo.build_config ());
    }
    catch (const io_error& e)
    {
      ostringstream os;
      os << "unable to read build configuration '" << bo.build_config ()
         << "': " << e;

      throw_generic_error (EIO, os.str ().c_str ());
    }

    if (bo.build_bot_agent_keys_specified ())
      bot_agent_key_map_ =
        shared_bot_agent_keys (bo, bo.build_bot_agent_keys ());

    using conf_map_type = map<build_target_config_id,
                              const build_target_config*>;

    conf_map_type conf_map;

    for (const auto& c: *target_conf_)
      conf_map[build_target_config_id {c.target, c.name}] = &c;

    target_conf_map_ = make_shared<conf_map_type> (move (conf_map));
  }

  bool build_config_module::
  belongs (const build_target_config& cfg, const char* cls) const
  {
    const map<string, string>& im (target_conf_->class_inheritance_map);

    for (const string& c: cfg.classes)
    {
      if (c == cls)
        return true;

      // Go through base classes.
      //
      for (auto i (im.find (c)); i != im.end (); )
      {
        const string& base (i->second);

        if (base == cls)
          return true;

        i = im.find (base);
      }
    }

    return false;
  }
}
