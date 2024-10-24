// file      : mod/ci-common.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_CI_COMMON_HXX
#define MOD_CI_COMMON_HXX

#include <odb/forward.hxx> // database

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <libbrep/build.hxx>
#include <libbrep/common.hxx>

#include <mod/diagnostics.hxx>
#include <mod/module-options.hxx>

namespace brep
{
  class ci_start
  {
  public:
    void
    init (shared_ptr<options::ci_start>);

    // If the request handling has been performed normally, then return the
    // information that corresponds to the CI result manifest (see CI Result
    // Manifest in the manual). Otherwise (some internal error has occured),
    // log the error and return nullopt.
    //
    // The arguments correspond to the CI request and overrides manifest
    // values (see CI Request and Overrides Manifests in the manual). Note:
    // request id and timestamp are generated by the implementation.
    //
    struct package
    {
      package_name name;
      optional<brep::version> version;
    };

    // Note that the inability to generate the reference is an internal
    // error. Thus, it is not optional.
    //
    // Note that if the CI request information is persisted to the database
    // (which, depending on the CI request handler, may not be the case), then
    // the reference is assumed to be the respective tenant id.
    //
    struct start_result
    {
      uint16_t status;
      string message;
      string reference;
      vector<pair<string, string>> custom_result;
    };

    // In the optional tenant service information, if service id is empty,
    // then the generated tenant id is used instead.
    //
    // Note that if the tenant service is specified, then the CI request
    // information is expected to be persisted to the database and thus
    // start_result::reference denotes the tenant id in this case (see above
    // for details).
    //
    optional<start_result>
    start (const basic_mark& error,
           const basic_mark& warn,
           const basic_mark* trace,
           optional<tenant_service>&&,
           const repository_location& repository,
           const vector<package>& packages,
           const optional<string>& client_ip,
           const optional<string>& user_agent,
           const optional<string>& interactive = nullopt,
           const optional<string>& simulate = nullopt,
           const vector<pair<string, string>>& custom_request = {},
           const vector<pair<string, string>>& overrides = {}) const;

    // Create an unloaded CI request returning tenant id on success and
    // nullopt on an internal error. Such a request is not started until
    // loaded with the load() function below. Configure the time interval
    // between the build_unloaded() notifications for the being created tenant
    // and set the initial delay for the first notification. See also the
    // build_unloaded() tenant services notification.
    //
    // The duplicate_tenant_mode argument specifies the behavior in case of
    // the duplicate tenant_service type/id pair. The default is to fail by
    // throwing an exception. Alternatively, this can be ignored or the
    // previous tenant can be canceled (thus freeing the type/id pair; see
    // below) and a new tenant with the same type/id created. In both these
    // modes (ignore and replace), the second half of the returned pair
    // indicates whether there was a duplicate. If there were, then for the
    // ignore mode the returned tenant id corresponds to the old tenant and
    // for the replace mode -- to the new tenant.
    //
    // The replace_archived mode is a variant of replace that replaces if the
    // tenant is already archived and ignores it otherwise (with the result
    // having the same semantics as in the replace and ignore modes).
    //
    // Note also that the duplicate_tenant_mode::replace modes are not the
    // same as separate calls to cancel() and then to create() since the
    // latter would happen in two separate transactions and will thus be racy.
    //
    // Finally note that only duplicate_tenant_mode::fail can be used if the
    // service id is empty.
    //
    // Note: should be called out of the database transaction.
    //
    enum class duplicate_tenant_mode {fail, ignore, replace, replace_archived};
    enum class duplicate_tenant_result {created, ignored, replaced};

    optional<pair<string, duplicate_tenant_result>>
    create (const basic_mark& error,
            const basic_mark& warn,
            const basic_mark* trace,
            odb::core::database&,
            tenant_service&&,
            duration notify_interval,
            duration notify_delay,
            duplicate_tenant_mode = duplicate_tenant_mode::fail) const;

    // Load (and start) previously created (as unloaded) CI request. Similarly
    // to the start() function, return nullopt on an internal error.
    //
    // Note that tenant_service::id is used to identify the CI request tenant.
    //
    // Note: should be called out of the database transaction.
    //
    optional<start_result>
    load (const basic_mark& error,
          const basic_mark& warn,
          const basic_mark* trace,
          odb::core::database&,
          tenant_service&&,
          const repository_location& repository) const;

    // Cancel previously created or started CI request. Return the service
    // state or nullopt if there is no tenant for such a type/id pair.
    //
    // Specifically, this function clears the tenant service state (thus
    // allowing reusing the same service type/id pair in another tenant) and
    // archives the tenant, unless the tenant is unloaded, in which case it is
    // dropped. Note that the latter allow using unloaded tenants as a
    // relatively cheap asynchronous execution mechanism.
    //
    // Note: should be called out of the database transaction.
    //
    optional<tenant_service>
    cancel (const basic_mark& error,
            const basic_mark& warn,
            const basic_mark* trace,
            odb::core::database&,
            const string& type,
            const string& id) const;

    // Cancel previously created or started CI request. Return false if there
    // is no tenant for the specified tenant id. Note that the reason argument
    // is only used for tracing.
    //
    // Similarly to above, this function archives the tenant, unless the
    // tenant is unloaded, in which case it is dropped. Note, however, that
    // this version does not touch the service state (use the above version if
    // you want to clear it).
    //
    // Note: should be called out of the database transaction.
    //
    bool
    cancel (const basic_mark& error,
            const basic_mark& warn,
            const basic_mark* trace,
            const string& reason,
            odb::core::database&,
            const string& tenant_id) const;

    // Schedule the re-build of the package build and return the build object
    // current state.
    //
    // Specifically:
    //
    // - If the build has expired (build or package object doesn't exist or
    //   the package is archived or is not buildable anymore, etc), then do
    //   nothing and return nullopt.
    //
    //   Note, however, that this function doesn't check if the build
    //   configuration still exists in the buildtab. It is supposed that the
    //   caller has already checked for that if necessary (see
    //   build_force::handle() for an example of this check). And if not
    //   then a re-build will be scheduled and later cleaned by the cleaner
    //   (without notifications).
    //
    // - Otherwise, if the build object is in the queued state, then do
    //   nothing and return build_state::queued. It is assumed that a build
    //   object in such a state is already about to be built.
    //
    // - Otherwise (the build object is in the building or built state),
    //   schedule the object for the rebuild and return the current state.
    //
    // Note that in contrast to the build-force handler, this function doesn't
    // send the build_queued() notification to the tenant-associated service
    // if the object is in the building state (which is done as soon as
    // possible to avoid races). Instead, it is assumed the service will
    // perform any equivalent actions directly based on the returned state.
    //
    // Note: should be called out of the database transaction.
    //
    optional<build_state>
    rebuild (odb::core::database&, const build_id&) const;

    // Find the tenant given the tenant service type and id and return the
    // associated data or nullopt if there is no such tenant.
    //
    // Note: should be called out of the database transaction.
    //
    optional<tenant_service>
    find (odb::core::database&,
          const string& type,
          const string& id) const;

    // Helpers.
    //

    // Serialize the start result as a CI result manifest.
    //
    static void
    serialize_manifest (const start_result&, ostream&, bool long_lines = false);

  private:
    shared_ptr<options::ci_start> options_;
  };
}

#endif // MOD_CI_COMMON_HXX
