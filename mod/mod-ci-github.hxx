// file      : mod/mod-ci-github.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_MOD_CI_GITHUB_HXX
#define MOD_MOD_CI_GITHUB_HXX

#include <web/xhtml/fragment.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/module.hxx>
#include <mod/module-options.hxx>

namespace butl
{
  namespace json
  {
    class parser;
  }
}

namespace brep
{
  // GitHub request/response types.
  //
  // Note that having this types directly in brep causes clashes (e.g., for
  // the repository name).
  //
  namespace gh
  {
    namespace json = butl::json;

    // The "check_suite" object within a check_suite webhook request.
    //
    struct check_suite
    {
      uint64_t id;
      string head_branch;
      string head_sha;
      string before;
      string after;

      explicit
      check_suite (json::parser&);

      check_suite () = default;
    };

    struct repository
    {
      string name;
      string full_name;
      string default_branch;

      explicit
      repository (json::parser&);

      repository () = default;
    };

    struct installation
    {
      uint64_t id;

      explicit
      installation (json::parser&);

      installation () = default;
    };

    struct check_suite_event
    {
      string action;
      gh::check_suite check_suite;
      gh::repository repository;
      gh::installation installation;

      explicit
      check_suite_event (json::parser&);

      check_suite_event () = default;
    };

    struct installation_access_token
    {
      string token;
      timestamp expires_at;

      explicit
      installation_access_token (json::parser&);

      installation_access_token () = default;
    };

    static ostream&
    operator<< (ostream&, const check_suite&);

    static ostream&
    operator<< (ostream&, const repository&);

    static ostream&
    operator<< (ostream&, const installation&);

    static ostream&
    operator<< (ostream&, const check_suite_event&);

    static ostream&
    operator<< (ostream&, const installation_access_token&);
  }

  class ci_github: public handler
  {
  public:
    ci_github () = default;

    // Create a shallow copy (handling instance) if initialized and a deep
    // copy (context exemplar) otherwise.
    //
    explicit
    ci_github (const ci_github&);

    virtual bool
    handle (request&, response&);

    virtual const cli::options&
    cli_options () const {return options::ci_github::description ();}

  private:
    virtual void
    init (cli::scanner&);

    // Handle the check_suite event `requested` and `rerequested` actions.
    //
    bool
    handle_check_suite_request (gh::check_suite_event) const;

    string
    generate_jwt () const;

    // Authenticate to GitHub as an app installation.
    //
    gh::installation_access_token
    obtain_installation_access_token (uint64_t install_id, string jwt) const;

  private:
    shared_ptr<options::ci_github> options_;
  };
}

#endif // MOD_MOD_CI_GITHUB_HXX
