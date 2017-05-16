// file      : mod/mod-repository-root.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-repository-root.hxx>

#include <time.h> // tzset()

#include <sstream>

#include <web/module.hxx>

#include <mod/module.hxx>
#include <mod/options.hxx>
#include <mod/mod-builds.hxx>
#include <mod/mod-build-log.hxx>
#include <mod/mod-build-task.hxx>
#include <mod/mod-build-force.hxx>
#include <mod/mod-build-result.hxx>
#include <mod/mod-package-search.hxx>
#include <mod/mod-package-details.hxx>
#include <mod/mod-repository-details.hxx>
#include <mod/mod-package-version-details.hxx>

using namespace std;
using namespace brep::cli;

namespace brep
{
  // request_proxy
  //
  class request_proxy: public request
  {
  public:
    request_proxy (request& r, const name_values& p)
        : request_ (r), parameters_ (p) {}

    virtual const path_type&
    path () {return request_.path ();}

    virtual const name_values&
    parameters () {return parameters_;}

    virtual const name_values&
    cookies () {return request_.cookies ();}

    virtual istream&
    content (size_t limit, size_t buffer) {
      return request_.content (limit, buffer);}

  private:
    request& request_;
    const name_values& parameters_;
  };

  // repository_root
  //
  repository_root::
  repository_root ()
      : package_search_ (make_shared<package_search> ()),
        package_details_ (make_shared<package_details> ()),
        package_version_details_ (make_shared<package_version_details> ()),
        repository_details_ (make_shared<repository_details> ()),
        build_task_ (make_shared<build_task> ()),
        build_result_ (make_shared<build_result> ()),
        build_force_ (make_shared<build_force> ()),
        build_log_ (make_shared<build_log> ()),
        builds_ (make_shared<builds> ())
  {
  }

  repository_root::
  repository_root (const repository_root& r)
      : module (r),
        //
        // Deep/shallow-copy sub-modules depending on whether this is an
        // exemplar/handler.
        //
        package_search_ (
          r.initialized_
          ? r.package_search_
          : make_shared<package_search> (*r.package_search_)),
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
          : make_shared<build_task> (*r.build_task_)),
        build_result_ (
          r.initialized_
          ? r.build_result_
          : make_shared<build_result> (*r.build_result_)),
        build_force_ (
          r.initialized_
          ? r.build_force_
          : make_shared<build_force> (*r.build_force_)),
        build_log_ (
          r.initialized_
          ? r.build_log_
          : make_shared<build_log> (*r.build_log_)),
        builds_ (
          r.initialized_
          ? r.builds_
          : make_shared<builds> (*r.builds_)),
        options_ (
          r.initialized_
          ? r.options_
          : nullptr)
  {
  }

  // Return amalgamation of repository_root and all its sub-modules option
  // descriptions.
  //
  option_descriptions repository_root::
  options ()
  {
    option_descriptions r (module::options ());
    append (r, package_search_->options ());
    append (r, package_details_->options ());
    append (r, package_version_details_->options ());
    append (r, repository_details_->options ());
    append (r, build_task_->options ());
    append (r, build_result_->options ());
    append (r, build_force_->options ());
    append (r, build_log_->options ());
    append (r, builds_->options ());
    return r;
  }

  // Initialize sub-modules and parse own configuration options.
  //
  void repository_root::
  init (const name_values& v)
  {
    auto sub_init = [this, &v] (module& m, const char* name)
    {
      // Initialize sub-module. Intercept exception handling to add sub-module
      // attribution.
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

    // Initialize sub-modules.
    //
    sub_init (*package_search_, "package_search");
    sub_init (*package_details_, "package_details");
    sub_init (*package_version_details_, "package_version_details");
    sub_init (*repository_details_, "repository_details");
    sub_init (*build_task_, "build_task");
    sub_init (*build_result_, "build_result");
    sub_init (*build_force_, "build_force");
    sub_init (*build_log_, "build_log");
    sub_init (*builds_, "builds");

    // Parse own configuration options.
    //
    module::init (
      filter (v, convert (options::repository_root::description ())));
  }

  void repository_root::
  init (scanner& s)
  {
    MODULE_DIAG;

    options_ = make_shared<options::repository_root> (
      s, unknown_mode::fail, unknown_mode::fail);

    if (options_->root ().empty ())
      options_->root (dir_path ("/"));

    // To use libbutl timestamp printing functions later on (specifically in
    // sub-modules, while handling requests).
    //
    tzset ();
  }

  bool repository_root::
  handle (request& rq, response& rs)
  {
    MODULE_DIAG;

    const dir_path& root (options_->root ());

    const path& rpath (rq.path ());
    if (!rpath.sub (root))
      return false;

    const path& lpath (rpath.leaf (root));

    // Delegate the request handling to the selected sub-module. Intercept
    // exception handling to add sub-module attribution.
    //
    auto handle = [&rs, this] (request& rq, const char* name) -> bool
    {
      try
      {
        return handler_->handle (rq, rs, *log_);
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
        // module::handle() function call.
        //
        ostringstream os;
        os << name << ": " << e;
        throw runtime_error (os.str ());
      }
    };

    // Note that while selecting the sub-module type for handling the request,
    // we rely on the fact that the initial and all the subsequent function
    // calls (that may take place after the retry exception is thrown) will
    // end-up with the same type, and so using the single handler instance for
    // all of these calls is safe. Note that the selection also sets up the
    // handling context (sub-module name and optionally the request proxy).
    //
    if (lpath.empty ())
    {
      // Dispatch request handling to the repository_details, the
      // package_search or the one of build_* modules depending on the function
      // name passed as a first HTTP request parameter. The parameter should
      // have no value specified. Example: cppget.org/?about
      //
      const name_values& params (rq.parameters ());
      if (!params.empty () && !params.front ().value)
      {
        // Cleanup not to confuse the selected module with the unknown
        // parameter.
        //
        name_values p (params);
        p.erase (p.begin ());

        request_proxy rp (rq, p);
        const string& fn (params.front ().name);

        if (fn == "about")
        {
          if (handler_ == nullptr)
            handler_.reset (new repository_details (*repository_details_));

          return handle (rp, "repository_details");
        }
        else if (fn == "build-task")
        {
          if (handler_ == nullptr)
            handler_.reset (new build_task (*build_task_));

          return handle (rp, "build_task");
        }
        else if (fn == "build-result")
        {
          if (handler_ == nullptr)
            handler_.reset (new build_result (*build_result_));

          return handle (rp, "build_result");
        }
        else if (fn == "build-force")
        {
          if (handler_ == nullptr)
            handler_.reset (new build_force (*build_force_));

          return handle (rp, "build_force");
        }
        else if (fn == "builds")
        {
          if (handler_ == nullptr)
            handler_.reset (new builds (*builds_));

          return handle (rp, "builds");
        }
        else
          throw invalid_request (400, "unknown function");
      }
      else
      {
        if (handler_ == nullptr)
          handler_.reset (new package_search (*package_search_));

        return handle (rq, "package_search");
      }
    }
    else
    {
      // Dispatch request handling to the package_details, the
      // package_version_details or the build_log module depending on the HTTP
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
      // @@ Shouldn't we validate that the package name is not "@", is not
      //    digit-only, does not start with '.' while parsing and serializing
      //    the package manifest ? Probably also need to mention these
      //    constraints in the manifest.txt file.
      //
      if (n != "@" && n.find_first_not_of ("0123456789") != string::npos &&
          n[0] != '.')
      {
        if (i == lpath.end ())
        {
          if (handler_ == nullptr)
            handler_.reset (new package_details (*package_details_));

          return handle (rq, "package_details");
        }
        else if (++i == lpath.end ())
        {
          if (handler_ == nullptr)
            handler_.reset (
              new package_version_details (*package_version_details_));

          return handle (rq, "package_version_details");
        }
        else if (*i == "log")
        {
          if (handler_ == nullptr)
            handler_.reset (new build_log (*build_log_));

          return handle (rq, "build_log");
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
    MODULE_DIAG;

    info << "module " << BREP_VERSION_ID
         << ", libbrep " << LIBBREP_VERSION_ID
         << ", libbbot " << LIBBBOT_VERSION_ID
         << ", libbpkg " << LIBBPKG_VERSION_ID
         << ", libbutl " << LIBBUTL_VERSION_ID;
  }
}
