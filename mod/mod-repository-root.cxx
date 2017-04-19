// file      : mod/mod-repository-root.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-repository-root>

#include <sstream>

#include <web/module>

#include <brep/version>

#include <mod/module>
#include <mod/options>
#include <mod/mod-package-search>
#include <mod/mod-package-details>
#include <mod/mod-repository-details>
#include <mod/mod-package-version-details>

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
        repository_details_ (make_shared<repository_details> ())
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
    return r;
  }

  // Initialize sub-modules and parse own configuration options.
  //
  void repository_root::
  init (const name_values& v)
  {
    auto sub_init ([this, &v](module& m)
      {
        m.init (filter (v, m.options ()), *log_);
      });

    // Initialize sub-modules.
    //
    sub_init (*package_search_);
    sub_init (*package_details_);
    sub_init (*package_version_details_);
    sub_init (*repository_details_);

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
    auto handle =
      [&rs, this] (request& rq, const char* name) -> bool
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
      // Dispatch request handling to the repository_details or the
      // package_search module depending on the function name passed as a
      // first HTTP request parameter. The parameter should have no value
      // specified. Example: cppget.org/?about
      //
      const name_values& params (rq.parameters ());
      if (!params.empty () && !params.front ().value)
      {
        if (params.front ().name == "about")
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
      // Dispatch request handling to the package_details or the
      // package_version_details module depending on the HTTP request URL path.
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

    info << "module " << BREP_VERSION_STR
         << ", libbrep " << LIBBREP_VERSION_STR
         << ", libbpkg " << LIBBPKG_VERSION_STR
         << ", libbutl " << LIBBUTL_VERSION_STR;
  }
}
