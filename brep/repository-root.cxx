// file      : brep/repository-root.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/repository-root>

#include <sstream>

#include <web/module>

#include <brep/types>
#include <brep/utility>

#include <brep/module>
#include <brep/options>
#include <brep/package-search>
#include <brep/package-details>
#include <brep/repository-details>
#include <brep/package-version-details>

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
    content () {return request_.content ();}

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

    static const dir_path& root (options_->root ());

    const path& rpath (rq.path ());
    if (!rpath.sub (root))
      return false;

    const path& lpath (rpath.leaf (root));

    // @@ An exception thrown by the selected module handle () function call
    //    will be attributed to the repository-root service while being logged.
    //    Could intercept exception handling to add some sub-module attribution,
    //    but let's not complicate the code for the time being.
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
          // Cleanup not to confuse the selected module with the unknown
          // parameter.
          //
          name_values p (params);
          p.erase (p.begin ());

          request_proxy rp (rq, p);
          repository_details m (*repository_details_);
          return m.handle (rp, rs);
        }

        throw invalid_request (400, "unknown function");
      }
      else
      {
        package_search m (*package_search_);
        return m.handle (rq, rs);
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
      // (CSS) directory name or a repository directory name.
      //
      if (n != "@" && n.find_first_not_of ("0123456789") != string::npos)
      {
        if (i == lpath.end ())
        {
          package_details m (*package_details_);
          return m.handle (rq, rs);
        }
        else if (++i == lpath.end ())
        {
          package_version_details m (*package_version_details_);
          return m.handle (rq, rs);
        }
      }
    }

    return false;
  }
}
