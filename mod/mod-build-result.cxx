// file      : mod/mod-build-result.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-build-result.hxx>

#include <algorithm> // find_if()

#include <libbutl/sendmail.hxx>
#include <libbutl/process-io.hxx>
#include <libbutl/manifest-parser.hxx>
#include <libbutl/manifest-serializer.hxx>

#include <libbbot/manifest.hxx>

#include <odb/database.hxx>
#include <odb/transaction.hxx>

#include <web/module.hxx>
#include <web/mime-url-encoding.hxx>

#include <libbrep/build.hxx>
#include <libbrep/build-odb.hxx>
#include <libbrep/package.hxx>
#include <libbrep/package-odb.hxx>

#include <mod/options.hxx>

using namespace std;
using namespace butl;
using namespace bbot;
using namespace web;
using namespace brep::cli;
using namespace odb::core;

// While currently the user-defined copy constructor is not required (we don't
// need to deep copy nullptr's), it is a good idea to keep the placeholder
// ready for less trivial cases.
//
brep::build_result::
build_result (const build_result& r)
    : database_module (r),
      options_ (r.initialized_ ? r.options_ : nullptr)
{
}

void brep::build_result::
init (scanner& s)
{
  MODULE_DIAG;

  options_ = make_shared<options::build_result> (
    s, unknown_mode::fail, unknown_mode::fail);

  database_module::init (static_cast<options::package_db> (*options_),
                         options_->package_db_retry ());

  if (options_->build_config_specified ())
    database_module::init (static_cast<options::build>    (*options_),
                           static_cast<options::build_db> (*options_),
                           options_->build_db_retry ());

  if (options_->root ().empty ())
    options_->root (dir_path ("/"));
}

bool brep::build_result::
handle (request& rq, response&)
{
  using brep::version; // Not to confuse with module::version.

  MODULE_DIAG;

  if (build_db_ == nullptr)
    throw invalid_request (501, "not implemented");

  // Make sure no parameters passed.
  //
  try
  {
    name_value_scanner s (rq.parameters ());
    params::build_result (s, unknown_mode::fail, unknown_mode::fail);
  }
  catch (const cli::exception& e)
  {
    throw invalid_request (400, e.what ());
  }

  result_request_manifest rqm;

  try
  {
    size_t limit (options_->build_result_request_max_size ());
    manifest_parser p (rq.content (limit, limit), "result_request_manifest");
    rqm = result_request_manifest (p);
  }
  catch (const manifest_parsing& e)
  {
    throw invalid_request (400, e.what ());
  }

  // Parse the task response session to obtain the build configuration name and
  // the timestamp, and to make sure the session matches the result manifest's
  // package name and version.
  //
  build_id id;
  timestamp session_timestamp;

  try
  {
    const string& s (rqm.session);

    size_t p (s.find ('/')); // End of package name.

    if (p == 0)
      throw invalid_argument ("empty package name");

    if (p == string::npos)
      throw invalid_argument ("no package version");

    string& name (rqm.result.name);
    if (name.compare (0, name.size (), s, 0, p) != 0)
      throw invalid_argument ("package name mismatch");

    size_t b (p + 1);    // Start of version.
    p = s.find ('/', b); // End of version.

    if (p == string::npos)
      throw invalid_argument ("no configuration name");

    version version;

    // Intercept exception handling to add the parsing error attribution.
    //
    try
    {
      version = brep::version (string (s, b, p - b));
    }
    catch (const invalid_argument& e)
    {
      throw invalid_argument (
        string ("invalid package version: ") + e.what ());
    }

    if (version != rqm.result.version)
      throw invalid_argument ("package version mismatch");

    b = p + 1;           // Start of configuration name.
    p = s.find ('/', b); // End of configuration name.

    if (p == string::npos)
      throw invalid_argument ("no timestamp");

    id = build_id (package_id (move (name), version), string (s, b, p - b));

    if (id.configuration.empty ())
      throw invalid_argument ("empty configuration name");

    try
    {
      size_t tsn;
      string ts (s, p + 1);

      session_timestamp = timestamp (
        chrono::duration_cast<timestamp::duration> (
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
  }
  catch (const invalid_argument& e)
  {
    throw invalid_request (400, string ("invalid session: ") + e.what ());
  }

  // If the session expired (no such configuration, package, etc), then we log
  // this case with the warning severity and respond with the 200 HTTP code as
  // if the session is valid. The thinking is that this is a problem with the
  // controller's setup (expires too fast), not with the agent's.
  //
  auto warn_expired = [&rqm, &warn] (const string& d)
  {
    warn << "session '" << rqm.session << "' expired: " << d;
  };

  // Make sure the build configuration still exists.
  //
  auto i (
    find_if (
      build_conf_->begin (), build_conf_->end (),
      [&id] (const build_config& c) {return c.name == id.configuration;}));

  if (i == build_conf_->end ())
  {
    warn_expired ("no build configuration");
    return true;
  }

  // Load the built package (if present).
  //
  shared_ptr<package> p;
  {
    transaction t (package_db_->begin ());
    p = package_db_->find<package> (id.package);
    t.commit ();
  }

  if (p == nullptr)
  {
    warn_expired ("no package");
    return true;
  }

  // Load and update the package build configuration (if present).
  //
  shared_ptr<build> b;
  optional<result_status> prev_status;
  bool notify (false);

  {
    transaction t (build_db_->begin ());
    b = build_db_->find<build> (id);

    if (b == nullptr)
      warn_expired ("no package configuration");
    else if (b->state != build_state::testing)
      warn_expired ("package configuration state is " + to_string (b->state));
    else if (b->timestamp != session_timestamp)
      warn_expired ("non-matching timestamp");
    else
    {
      // Don's send email for the success-to-success status change, unless the
      // build was forced.
      //
      notify = !(rqm.result.status == result_status::success &&
                 b->status && *b->status == rqm.result.status && !b->forced);

      prev_status = move (b->status);

      b->state = build_state::tested;
      b->status = rqm.result.status;
      b->forced = false;

      // Mark the section as loaded, so results are updated.
      //
      b->results_section.load ();
      b->results = move (rqm.result.results);

      b->timestamp = timestamp::clock::now ();

      build_db_->update (b);
    }

    t.commit ();
  }

  // Don't send the notification email if the empty package build email is
  // specified.
  //
  const optional<email>& build_email (p->build_email);
  if (!notify || (build_email && build_email->empty ()))
    return true;

  assert (b != nullptr);

  // Send email to the package owner.
  //
  try
  {
    string subj (to_string (*b->status) + ": " + b->package_name + '/' +
                 b->package_version.string () + '/' + b->configuration);

    // If the package build address is not specified, then it is assumed to be
    // the same as the package email address, if specified, otherwise as the
    // project email address.
    //
    const string& to (build_email ? *build_email
                      : p->package_email
                        ? *p->package_email
                        : p->email);

    auto print_args = [&trace, this] (const char* args[], size_t n)
    {
      l2 ([&]{trace << process_args {args, n};});
    };

    // Redirect the diagnostics to webserver error log.
    //
    // Note: if using this somewhere else, then need to factor out all this
    // exit status handling code.
    //
    sendmail sm (print_args,
                 2,
                 options_->email (),
                 subj,
                 {to});

    if (b->results.empty ())
      sm.out << "No operations results available." << endl;
    else
    {
      string url (options_->host () + options_->root ().representation ());
      string pkg (mime_url_encode (b->package_name));
      string cfg (mime_url_encode (b->configuration));

      // Note that '+' is the only package version character that potentially
      // needs to be url-encoded, and only in the query part of the URL.
      // However, we print the package version either as part of URL path or
      // as the build-force URL query part (where it is not encoded by
      // design).
      //
      const version& ver (b->package_version);
      ostream& os (sm.out);

      assert (b->status);
      os << "combined: " << *b->status << endl << endl
         << "  " << url << pkg << '/' << ver << "/log/" << cfg << endl << endl;

      for (const auto& r: b->results)
        os << r.operation << ": " << r.status << endl << endl
           << "  " << url << pkg << '/' << ver << "/log/" << cfg << '/'
           << r.operation << endl << endl;

      os << "Force rebuild (enter the reason, use '+' instead of spaces):"
         << endl << endl
         << "  " << options_->host () << options_->root () << "?build-force&p="
         << pkg << "&v=" << ver << "&c=" << cfg << "&reason=" << endl;
    }

    sm.out.close ();

    if (!sm.wait ())
    {
      diag_record dr (error);
      dr << "sendmail ";

      assert (sm.exit);
      const process_exit& e (*sm.exit);

      if (e.normal ())
        dr << "exited with code " << static_cast<uint16_t> (e.code ());
      else
      {
        dr << "terminated abnormally: " << e.description ();

        if (e.core ())
          dr << " (core dumped)";
      }
    }
  }
  // Handle process_error and io_error (both derive from system_error).
  //
  catch (const system_error& e)
  {
    error << "sendmail error: " << e;
  }

  return true;
}
