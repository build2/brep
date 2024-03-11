// file      : mod/build.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/build.hxx>

#include <odb/database.hxx>
#include <odb/connection.hxx>
#include <odb/transaction.hxx>

#include <libbutl/sendmail.hxx>
#include <libbutl/process-io.hxx>

#include <web/server/mime-url-encoding.hxx>

#include <libbrep/build-package-odb.hxx>

#include <mod/utility.hxx>

namespace brep
{
  using namespace std;
  using namespace web;

  string
  build_log_url (const string& host, const dir_path& root,
                 const build& b,
                 const string* op)
  {
    // Note that '+' is the only package version character that potentially
    // needs to be url-encoded, and only in the query part of the URL. We embed
    // the package version into the URL path part and so don't encode it.
    //
    string url (
      host + tenant_dir (root, b.tenant).representation ()             +
      mime_url_encode (b.package_name.string (), false) + '/'          +
      b.package_version.string () + "/log/"                            +
      mime_url_encode (b.target.string (), false /* query */) + '/'    +
      mime_url_encode (b.target_config_name, false /* query */) + '/'  +
      mime_url_encode (b.package_config_name, false /* query */) + '/' +
      mime_url_encode (b.toolchain_name, false /* query */) + '/'      +
      b.toolchain_version.string ());

    if (op != nullptr)
    {
      url += '/';
      url += *op;
    }

    return url;
  }

  string
  build_force_url (const string& host, const dir_path& root, const build& b)
  {
    // Note that '+' is the only package version character that potentially
    // needs to be url-encoded, and only in the query part of the URL. However
    // we embed the package version into the URL query part, where it is not
    // encoded by design.
    //
    return host + tenant_dir (root, b.tenant).string ()               +
      "?build-force&pn=" + mime_url_encode (b.package_name.string ()) +
      "&pv=" + b.package_version.string ()                            +
      "&tg=" + mime_url_encode (b.target.string ())                   +
      "&tc=" + mime_url_encode (b.target_config_name)                 +
      "&pc=" + mime_url_encode (b.package_config_name)                +
      "&tn=" + mime_url_encode (b.toolchain_name)                     +
      "&tv=" + b.toolchain_version.string ()                          +
      "&reason=";
  }

  void
  send_notification_email (const options::build_email_notification& o,
                           const odb::core::connection_ptr& conn,
                           const build& b,
                           const build_package& p,
                           const build_package_config& pc,
                           const string& what,
                           const basic_mark& error,
                           const basic_mark* trace)
  {
    using namespace odb::core;
    using namespace butl;

    assert (b.state == build_state::built && b.status);

    // Bail out if sending build notification emails is disabled for this
    // toolchain for this package.
    //
    {
      const map<string, build_email>& tes (o.build_toolchain_email ());
      auto i (tes.find (b.id.toolchain_name));
      build_email mode (i != tes.end () ? i->second : build_email::latest);

      if (mode == build_email::none)
      {
        return;
      }
      else if (mode == build_email::latest)
      {
        transaction t (conn->begin ());
        database& db (t.database ());

        const auto& id (query<buildable_package>::build_package::id);

        buildable_package lp (
          db.query_value<buildable_package> (
            (id.tenant == b.tenant && id.name == b.package_name) +
            order_by_version_desc (id.version)                   +
            "LIMIT 1"));

        t.commit ();

        if (lp.package->version != p.version)
          return;
      }
    }

    string subj (what + ' '                        +
                 to_string (*b.status) + ": "      +
                 b.package_name.string () + '/'    +
                 b.package_version.string () + ' ' +
                 b.target_config_name + '/'        +
                 b.target.string () + ' '          +
                 b.package_config_name + ' '       +
                 b.toolchain_name + '-' + b.toolchain_version.string ());

    // Send notification emails to the interested parties.
    //
    auto send_email = [&b, &subj, &o, &error, trace] (const string& to)
    {
      try
      {
        if (trace != nullptr)
          *trace << "email '" << subj << "' to " << to;

        // Redirect the diagnostics to webserver error log.
        //
        sendmail sm ([trace] (const char* args[], size_t n)
                     {
                       if (trace != nullptr)
                         *trace << process_args {args, n};
                     },
                     2,
                     o.email (),
                     subj,
                     {to});

        if (b.results.empty ())
        {
          sm.out << "No operation results available." << endl;
        }
        else
        {
          const string& host (o.host ());
          const dir_path& root (o.root ());

          ostream& os (sm.out);

          os << "combined: " << *b.status << endl << endl
             << "  " << build_log_url (host, root, b) << endl << endl;

          for (const auto& r: b.results)
            os << r.operation << ": " << r.status << endl << endl
               << "  " << build_log_url (host, root, b, &r.operation)
               << endl << endl;

          os << "Force rebuild (enter the reason, use '+' instead of spaces):"
             << endl << endl
             << "  " << build_force_url (host, root, b) << endl;
        }

        sm.out.close ();

        if (!sm.wait ())
          error << "sendmail " << *sm.exit;
      }
      // Handle process_error and io_error (both derive from system_error).
      //
      catch (const system_error& e)
      {
        error << "sendmail error: " << e;
      }
    };

    // Send the build notification email if a non-empty package build email is
    // specified.
    //
    if (const optional<email>& e = pc.effective_email (p.build_email))
    {
      if (!e->empty ())
        send_email (*e);
    }

    // Send the build warning/error notification emails, if requested.
    //
    if (*b.status >= result_status::warning)
    {
      if (const optional<email>& e =
          pc.effective_warning_email (p.build_warning_email))
        send_email (*e);
    }

    if (*b.status >= result_status::error)
    {
      if (const optional<email>& e =
          pc.effective_error_email (p.build_error_email))
        send_email (*e);
    }
  }
}
