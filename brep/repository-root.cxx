// file      : brep/repository-root.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/repository-root>

#include <map>
#include <functional>

#include <web/module>

#include <brep/types>
#include <brep/utility>
#include <brep/package-search>
#include <brep/repository-details>

using namespace std;

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
  void repository_root::
  handle (request& rq, response& rs)
  {
    MODULE_DIAG;

    // Dispatch request handling to the appropriate module depending on the
    // function name passed as a first HTTP request parameter. The parameter
    // should have no value specified. If no function name is passed,
    // the default handler is selected. Example: cppget.org/?about
    //

    string func;
    name_values params (rq.parameters ());

    // Obtain the function name.
    //
    if (!params.empty () && !params.front ().value)
    {
      func = move (params.front ().name);

      // Cleanup not to confuse the selected handler with the unknown parameter.
      //
      params.erase (params.begin ());
    }

    // To handle the request a new module instance is created as a copy of
    // the corresponsing exemplar.
    //
    using module_ptr = unique_ptr<module>;

    // Function name to module factory map.
    //
    const map<string, function<module_ptr()>>
      handlers ({
        {
          "about",
          [this]() -> module_ptr
          {return module_ptr (new repository_details (repository_details_));}
        },
        {
          string (), // The default handler.
          [this]() -> module_ptr
          {return module_ptr (new package_search (package_search_));}
        }});

    // Find proper handler.
    //
    auto i (handlers.find (func));
    if (i == handlers.end ())
      throw invalid_request (400, "unknown function");

    module_ptr m (i->second ());
    if (m->loaded ())
    {
      // Delegate request handling.
      //
      // @@ An exception thrown by the handler will be attributed to the
      //    repository-root service while being logged. Could intercept
      //    exception handling to fix that, but let's not complicate the
      //    code for the time being.
      //
      //
      request_proxy rqp (rq, params);
      m->handle (rqp, rs);
    }
    else
      // The module is not loaded, presumably being disabled in the web server
      // configuration file.
      //
      throw invalid_request (404, "handler not available");
  }
}
