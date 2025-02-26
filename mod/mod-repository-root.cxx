// file      : mod/mod-repository-root.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-repository-root.hxx>

#include <time.h> // tzset()

#include <cmark-gfm-core-extensions.h>

#include <sstream>

#include <web/server/module.hxx>

#include <mod/module.hxx>
#include <mod/module-options.hxx>

#include <mod/mod-ci.hxx>
#include <mod/mod-ci-github.hxx>
#include <mod/mod-submit.hxx>
#include <mod/mod-upload.hxx>
#include <mod/mod-builds.hxx>
#include <mod/mod-packages.hxx>
#include <mod/mod-build-log.hxx>
#include <mod/mod-build-task.hxx>
#include <mod/mod-build-force.hxx>
#include <mod/mod-build-result.hxx>
#include <mod/mod-build-configs.hxx>
#include <mod/mod-package-details.hxx>
#include <mod/mod-advanced-search.hxx>
#include <mod/mod-repository-details.hxx>
#include <mod/mod-package-version-details.hxx>

using namespace std;
using namespace brep::cli;

namespace brep
{
  // Request proxy.
  //
  // Removes the first parameter, that is assumed to be a function name, if its
  // value is not present. Otherwise, considers it as the handler's "default"
  // parameter value and renames the parameter to "_".
  //
  class request_proxy: public request
  {
  public:
    request_proxy (request& r): request_ (r) {}

    virtual const path_type&
    path () {return request_.path ();}

    virtual const name_values&
    parameters (size_t limit, bool url_only)
    {
      if (!parameters_ || url_only < url_only_parameters_)
      {
        parameters_ = request_.parameters (limit, url_only);
        assert (!parameters_->empty ()); // Always starts with a function name.

        auto i (parameters_->begin ());
        removed_ = !i->value;

        if (removed_)
          parameters_->erase (i);
        else
          i->name = "_";

        url_only_parameters_ = url_only;
      }

      return *parameters_;
    }

    istream&
    open_upload (size_t index)
    {
      // Shift the index if the function name parameter was removed.
      //
      return request_.open_upload (index + (removed_ ? 1 : 0));
    }

    istream&
    open_upload (const string& name)
    {
      // We don't expect the function name here as a parameter name.
      //
      return request_.open_upload (name);
    }

    virtual const name_values&
    headers () {return request_.headers ();}

    virtual const name_values&
    cookies () {return request_.cookies ();}

    virtual istream&
    content (size_t limit, size_t buffer)
    {
      return request_.content (limit, buffer);
    }

  private:
    request& request_;
    optional<name_values> parameters_;
    bool url_only_parameters_; // Meaningless if parameters_ is not present.
    bool removed_ = false;     // If the function name parameter was removed.
  };

  // repository_root
  //
  repository_root::
  repository_root ()
      :
        //
        // Only create and populate the tenant service map in the examplar
        // passing a reference to it to all the sub-handler exemplars. Note
        // that we dispatch the tenant service callbacks to the examplar
        // without creating a new instance for each callback (thus the
        // callbacks are const).
        //
        tenant_service_map_ (make_shared<tenant_service_map> ()),
        packages_ (make_shared<packages> ()),
        advanced_search_ (make_shared<advanced_search> ()),
        package_details_ (make_shared<package_details> ()),
        package_version_details_ (make_shared<package_version_details> ()),
        repository_details_ (make_shared<repository_details> ()),
        build_task_ (make_shared<build_task> (*tenant_service_map_)),
        build_result_ (make_shared<build_result> (*tenant_service_map_)),
        build_force_ (make_shared<build_force> (*tenant_service_map_)),
        build_log_ (make_shared<build_log> ()),
        builds_ (make_shared<builds> ()),
        build_configs_ (make_shared<build_configs> ()),
        submit_ (make_shared<submit> ()),
#ifdef BREP_CI_TENANT_SERVICE
        ci_ (make_shared<ci> (*tenant_service_map_)),
#else
        ci_ (make_shared<ci> ()),
#endif
        ci_cancel_ (make_shared<ci_cancel> ()),
        ci_github_ (make_shared<ci_github> (*tenant_service_map_)),
        upload_ (make_shared<upload> ())
  {
  }

  repository_root::
  repository_root (const repository_root& r)
      : handler (r),
        tenant_service_map_ (
          r.initialized_
          ? r.tenant_service_map_
          : make_shared<tenant_service_map> ()),
        //
        // Deep/shallow-copy sub-handlers depending on whether this is an
        // exemplar/handler.
        //
        packages_ (
          r.initialized_
          ? r.packages_
          : make_shared<packages> (*r.packages_)),
        advanced_search_ (
          r.initialized_
          ? r.advanced_search_
          : make_shared<advanced_search> (*r.advanced_search_)),
        package_details_ (
          r.initialized_
          ? r.package_details_
          : make_shared<package_details> (*r.package_details_)),
        package_version_details_ (
          r.initialized_
          ? r.package_version_details_
          : make_shared<package_version_details> (
              *r.package_version_details_)),
        repository_details_ (
          r.initialized_
          ? r.repository_details_
          : make_shared<repository_details> (*r.repository_details_)),
        build_task_ (
          r.initialized_
          ? r.build_task_
          : make_shared<build_task> (*r.build_task_, *tenant_service_map_)),
        build_result_ (
          r.initialized_
          ? r.build_result_
          : make_shared<build_result> (*r.build_result_, *tenant_service_map_)),
        build_force_ (
          r.initialized_
          ? r.build_force_
          : make_shared<build_force> (*r.build_force_, *tenant_service_map_)),
        build_log_ (
          r.initialized_
          ? r.build_log_
          : make_shared<build_log> (*r.build_log_)),
        builds_ (
          r.initialized_
          ? r.builds_
          : make_shared<builds> (*r.builds_)),
        build_configs_ (
          r.initialized_
          ? r.build_configs_
          : make_shared<build_configs> (*r.build_configs_)),
        submit_ (
          r.initialized_
          ? r.submit_
          : make_shared<submit> (*r.submit_)),
        ci_ (
          r.initialized_
          ? r.ci_
#ifdef BREP_CI_TENANT_SERVICE
          : make_shared<ci> (*r.ci_, *tenant_service_map_)),
#else
          : make_shared<ci> (*r.ci_)),
#endif
        ci_cancel_ (
          r.initialized_
          ? r.ci_cancel_
          : make_shared<ci_cancel> (*r.ci_cancel_)),
        ci_github_ (
          r.initialized_
          ? r.ci_github_
          : make_shared<ci_github> (*r.ci_github_, *tenant_service_map_)),
        upload_ (
          r.initialized_
          ? r.upload_
          : make_shared<upload> (*r.upload_)),
        options_ (
          r.initialized_
          ? r.options_
          : nullptr)
  {
  }

  // Return amalgamation of repository_root and all its sub-handlers option
  // descriptions.
  //
  option_descriptions repository_root::
  options ()
  {
    option_descriptions r (handler::options ());
    append (r, packages_->options ());
    append (r, advanced_search_->options ());
    append (r, package_details_->options ());
    append (r, package_version_details_->options ());
    append (r, repository_details_->options ());
    append (r, build_task_->options ());
    append (r, build_result_->options ());
    append (r, build_force_->options ());
    append (r, build_log_->options ());
    append (r, builds_->options ());
    append (r, build_configs_->options ());
    append (r, submit_->options ());
    append (r, ci_->options ());
    append (r, ci_cancel_->options ());
    append (r, ci_github_->options ());
    append (r, upload_->options ());
    return r;
  }

  // Initialize sub-handlers and parse own configuration options.
  //
  void repository_root::
  init (const name_values& v)
  {
    auto sub_init = [this, &v] (handler& m, const char* name)
    {
      // Initialize sub-handler. Intercept exception handling to add
      // sub-handler attribution.
      //
      try
      {
        m.init (filter (v, m.options ()), *log_);
      }
      catch (const std::exception& e)
      {
        // Any exception thrown by this function terminates the web server. All
        // exception types inherited from std::exception are handled by the web
        // server as std::exception. The only sensible way to handle them is to
        // log the error prior terminating. By that reason it is valid to
        // reduce all these types to a single one.
        //
        ostringstream os;
        os << name << ": " << e;
        throw runtime_error (os.str ());
      }
    };

    // Initialize sub-handlers.
    //
    sub_init (*packages_, "packages");
    sub_init (*advanced_search_, "advanced_search");
    sub_init (*package_details_, "package_details");
    sub_init (*package_version_details_, "package_version_details");
    sub_init (*repository_details_, "repository_details");
    sub_init (*build_task_, "build_task");
    sub_init (*build_result_, "build_result");
    sub_init (*build_force_, "build_force");
    sub_init (*build_log_, "build_log");
    sub_init (*builds_, "builds");
    sub_init (*build_configs_, "build_configs");
    sub_init (*submit_, "submit");
    sub_init (*ci_, "ci");
    sub_init (*ci_cancel_, "ci-cancel");
    sub_init (*ci_github_, "ci_github");
    sub_init (*upload_, "upload");

    // Parse own configuration options.
    //
    handler::init (
      filter (v, convert (options::repository_root::description ())));
  }

  void repository_root::
  init (scanner& s)
  {
    HANDLER_DIAG;

    options_ = make_shared<options::repository_root> (
      s, unknown_mode::fail, unknown_mode::fail);

    // Verify that the root default views are properly configured.
    //
    auto verify = [&fail] (const string& v, const char* what)
    {
      cstrings vs ({
          "packages",
          "advanced-search",
          "builds",
          "build-configs",
          "about",
          "submit",
          "ci",
          "ci-github"});

      if (find (vs.begin (), vs.end (), v) == vs.end ())
        fail << what << " value '" << v << "' is invalid";
    };

    verify (options_->root_global_view (), "root-global-view");
    verify (options_->root_tenant_view (), "root-tenant-view");

    if (options_->root ().empty ())
      options_->root (dir_path ("/"));

    // To use libbutl timestamp printing functions later on (specifically in
    // sub-handlers, while handling requests).
    //
    tzset ();

    // To recognize cmark-gfm extensions while parsing Markdown later on.
    //
    cmark_gfm_core_extensions_ensure_registered ();
  }

  bool repository_root::
  handle (request& rq, response& rs)
  {
    HANDLER_DIAG;

    const dir_path& root (options_->root ());

    const path& rpath (rq.path ());
    if (!rpath.sub (root))
      return false;

    path lpath (rpath.leaf (root));

    if (!lpath.empty ())
    {
      path::iterator i (lpath.begin ());
      const string& s (*i);

      if (s[0] == '@' && s.size () > 1)
      {
        tenant = string (s, 1);
        lpath = path (++i, lpath.end ());
      }
    }

    // Delegate the request handling to the selected sub-handler. Intercept
    // exception handling to add sub-handler attribution.
    //
    auto handle = [&rq, &rs, this] (const char* nm, bool fn = false)
    {
      handler_->tenant = move (tenant);

      try
      {
        // Delegate the handling straight away if the sub-handler is not a
        // function. Otherwise, cleanup the request not to confuse the
        // sub-handler with the unknown parameter.
        //
        if (!fn)
          return handler_->handle (rq, rs, *log_);

        request_proxy rp (rq);
        return handler_->handle (rp, rs, *log_);
      }
      catch (const invalid_request&)
      {
        // Preserve invalid_request exception type, so the web server can
        // properly respond to the client with a 4XX error code.
        //
        throw;
      }
      catch (const std::exception& e)
      {
        // All exception types inherited from std::exception (and different
        // from invalid_request) are handled by the web server as
        // std::exception. The only sensible way to handle them is to respond
        // to the client with the internal server error (500) code. By that
        // reason it is valid to reduce all these types to a single one. Note
        // that the server_error exception is handled internally by the
        // handler::handle() function call.
        //
        ostringstream os;
        os << nm << ": " << e;
        throw runtime_error (os.str ());
      }
    };

    // Note that while selecting the sub-handler type for handling the request,
    // we rely on the fact that the initial and all the subsequent function
    // calls (that may take place after the retry exception is thrown) will
    // end-up with the same type, and so using the single handler instance for
    // all of these calls is safe. Note that the selection also sets up the
    // handling context (sub-handler name and optionally the request proxy).
    //
    if (lpath.empty ())
    {
      // Dispatch request handling to one of the sub-handlers depending on the
      // function name passed as a first HTTP request parameter (example:
      // cppget.org/?about). If it doesn't denote a handler or there are no
      // parameters, then dispatch to the default handler.
      //
      const name_values& params (rq.parameters (0 /* limit */,
                                                true /* url_only */));

      auto dispatch = [&handle, this] (const string& func,
                                       bool param) -> optional<bool>
      {
        // When adding a new handler don't forget to check if need to add it
        // to the default view list in the init() function.
        //
        if (func == "build-task")
        {
          if (handler_ == nullptr)
            handler_.reset (new build_task (*build_task_));

          return handle ("build_task", param);
        }
        else if (func == "build-result")
        {
          if (handler_ == nullptr)
            handler_.reset (new build_result (*build_result_));

          return handle ("build_result", param);
        }
        else if (func == "build-force")
        {
          if (handler_ == nullptr)
            handler_.reset (new build_force (*build_force_));

          return handle ("build_force", param);
        }
        else if (func == "builds")
        {
          if (handler_ == nullptr)
            handler_.reset (new builds (*builds_));

          return handle ("builds", param);
        }
        else if (func == "build-configs")
        {
          if (handler_ == nullptr)
            handler_.reset (new build_configs (*build_configs_));

          return handle ("build_configs", param);
        }
        else if (func == "packages")
        {
          if (handler_ == nullptr)
            handler_.reset (new packages (*packages_));

          return handle ("packages", param);
        }
        else if (func == "advanced-search")
        {
          if (handler_ == nullptr)
            handler_.reset (new advanced_search (*advanced_search_));

          return handle ("advanced_search", param);
        }
        else if (func == "about")
        {
          if (handler_ == nullptr)
            handler_.reset (new repository_details (*repository_details_));

          return handle ("repository_details", param);
        }
        else if (func == "submit")
        {
          if (handler_ == nullptr)
            handler_.reset (new submit (*submit_));

          return handle ("submit", param);
        }
        else if (func == "ci")
        {
          if (handler_ == nullptr)
            handler_.reset (new ci (*ci_));

          return handle ("ci", param);
        }
        else if (func == "ci-cancel")
        {
          if (handler_ == nullptr)
            handler_.reset (new ci_cancel (*ci_cancel_));

          return handle ("ci-cancel", param);
        }
        else if (func == "ci-github")
        {
          if (handler_ == nullptr)
            handler_.reset (new ci_github (*ci_github_));

          return handle ("ci_github", param);
        }
        else if (func == "upload")
        {
          if (handler_ == nullptr)
            handler_.reset (new upload (*upload_));

          return handle ("upload", param);
        }
        else
          return nullopt;
      };

      optional<bool> r;

      if (!params.empty () &&
          (r = dispatch (params.front ().name, true /* param */)))
        return *r;

      const string& view (!tenant.empty ()
                          ? options_->root_tenant_view ()
                          : options_->root_global_view ());

      r = dispatch (view, false /* param */);

      // The default views are verified in the init() function.
      //
      assert (r);

      return *r;
    }
    else
    {
      // Dispatch request handling to the package_details, the
      // package_version_details or the build_log handler depending on the HTTP
      // request URL path.
      //
      auto i (lpath.begin ());
      assert (i != lpath.end ());

      const string& n (*i++); // Package name.

      // Check if this is a package name and not a brep static content files
      // (CSS) directory name, a repository directory name, or a special file
      // name (the one starting with '.'). Note that HTTP request URL path
      // (without the root directory) must also have one of the following
      // layouts:
      //
      // <package-name>
      // <package-name>/<package-version>
      // <package-name>/<package-version>/log[/...]
      //
      // If any of the checks fails, then the handling is declined.
      //
      if (n != "@" && n.find_first_not_of ("0123456789") != string::npos &&
          n[0] != '.')
      {
        if (i == lpath.end ())
        {
          if (handler_ == nullptr)
            handler_.reset (new package_details (*package_details_));

          return handle ("package_details");
        }
        else if (++i == lpath.end ())
        {
          if (handler_ == nullptr)
            handler_.reset (
              new package_version_details (*package_version_details_));

          return handle ("package_version_details");
        }
        else if (*i == "log")
        {
          if (handler_ == nullptr)
            handler_.reset (new build_log (*build_log_));

          return handle ("build_log");
        }
      }
    }

    // We shouldn't be selecting a handler if decline to handle the request.
    //
    assert (handler_ == nullptr);
    return false;
  }

  void repository_root::
  version ()
  {
    HANDLER_DIAG;

    info << "module " << BREP_VERSION_ID
         << ", libbrep " << LIBBREP_VERSION_ID
         << ", libbbot " << LIBBBOT_VERSION_ID
         << ", libbpkg " << LIBBPKG_VERSION_ID
         << ", libbutl " << LIBBUTL_VERSION_ID;
  }
}
