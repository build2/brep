// file      : mod/tenant-service.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_TENANT_SERVICE_HXX
#define MOD_TENANT_SERVICE_HXX

#include <map>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <libbrep/build.hxx>

#include <mod/diagnostics.hxx>

namespace brep
{
  class tenant_service_base
  {
  public:
    virtual ~tenant_service_base () = default;
  };

  // Possible build notifications (see also the unloaded special notification
  // below):
  //
  // queued
  // building
  // built
  //
  // Possible transitions:
  //
  //          -> queued
  // queued   -> building
  // building -> queued   (interrupted & re-queued due to higher priority task)
  // building -> built
  // built    -> queued   (periodic or user-forced rebuild)
  //
  // While the implementation tries to make sure the notifications arrive in
  // the correct order, this is currently done by imposing delays (some
  // natural, such as building->built, and some artificial, such as
  // queued->building). As result, it is unlikely but possible to observe the
  // state transition notifications in the wrong order, especially if
  // processing notifications can take a long time. For example, while
  // processing the queued notification, the building notification may arrive
  // in a different thread. To minimize the chance of this happening, the
  // service implementation should strive to batch the queued state
  // notifications (of which there could be hundreds) in a single request if
  // at all possible. Also, if supported by the third-party API, it makes
  // sense for the implementation to protect against overwriting later states
  // with earlier. For example, if it's possible to place a condition on a
  // notification, it makes sense to only set the state to queued if none of
  // the later states (e.g., building) are already in effect. See also
  // ci_start::rebuild() for additional details on the build->queued
  // transition.
  //
  // Note also that it's possible for the build to get deleted at any stage
  // without any further notifications. This can happen, for example, due to
  // data retention timeout or because the build configuration (buildtab
  // entry) is no longer present. There is no explicit `deleted` transition
  // notification because such situations (i.e., when a notification sequence
  // is abandoned half way) are not expected to arise ordinarily in a
  // properly-configured brep instance. And the third-party service is
  // expected to deal with them using some overall timeout/expiration
  // mechanism which it presumably has.
  //
  // Each build notification is in its own interface since a service may not
  // be interested in all of them while computing the information to pass is
  // expensive.

  class tenant_service_build_queued: public virtual tenant_service_base
  {
  public:
    // If the returned function is not NULL, it is called to update the
    // service data. It should return the new data or nullopt if no update is
    // necessary. Note: tenant_service::data passed to the callback and to the
    // returned function may not be the same. Furthermore, tenant_ids may not
    // be the same either, in case the tenant was replaced. Also, the returned
    // function may be called multiple times (on transaction retries). Note
    // that the passed log_writer is valid during the calls to the returned
    // function.
    //
    // The passed initial_state indicates the logical initial state and is
    // either absent, `building` (interrupted), or `built` (rebuild). Note
    // that all the passed build objects are for the same package version and
    // have the same initial state.
    //
    // The implementation of this and the below functions should normally not
    // need to make any decisions based on the passed build::state. Rather,
    // the function name suffix (_queued, _building, _built) signify the
    // logical end state.
    //
    // The build_queued_hints can be used to omit certain components from the
    // build id. If single_package_version is true, then this tenant contains
    // a single (non-test) package version and this package name and package
    // version can be omitted. If single_package_config is true, then the
    // package version being built only has the default package configuration
    // and thus it can be omitted.
    //
    struct build_queued_hints
    {
      bool single_package_version;
      bool single_package_config;
    };

    virtual function<optional<string> (const string& tenant_id,
                                       const tenant_service&)>
    build_queued (const string& tenant_id,
                  const tenant_service&,
                  const vector<build>&,
                  optional<build_state> initial_state,
                  const build_queued_hints&,
                  const diag_epilogue& log_writer) const noexcept = 0;
  };

  class tenant_service_build_building: public virtual tenant_service_base
  {
  public:
    virtual function<optional<string> (const string& tenant_id,
                                       const tenant_service&)>
    build_building (const string& tenant_id,
                    const tenant_service&,
                    const build&,
                    const diag_epilogue& log_writer) const noexcept = 0;
  };

  class tenant_service_build_built: public virtual tenant_service_base
  {
  public:
    // The second half of the pair signals whether to call the
    // build_completed() notification.
    //
    virtual function<pair<optional<string>, bool> (const string& tenant_id,
                                                   const tenant_service&)>
    build_built (const string& tenant_id,
                 const tenant_service&,
                 const build&,
                 const diag_epilogue& log_writer) const noexcept = 0;

    virtual void
    build_completed (const string& tenant_id,
                     const tenant_service&,
                     const diag_epilogue& log_writer) const noexcept;

    // Called when the tenant is archived due to the inability to save service
    // data (for example, due to persistent transaction rollbacks). Note that
    // the passed tenant_service argument contains the unsaved service data
    // (while the tenant still contains the original data; note that this
    // behavior is unlike explicit cancellation via ci_start::cancel()). Note
    // also that this function is not called when the tenant is canceled
    // explicitly with the ci_start::cancel() functions.
    //
    virtual void
    build_canceled (const string& tenant_id,
                    const tenant_service&,
                    const diag_epilogue& log_writer) const noexcept;
  };

  // This notification is only made on unloaded CI requests created with the
  // ci_start::create() call and until they are loaded with ci_start::load()
  // or, alternatively, abandoned with ci_start::cancel() (in which case the
  // returned callback should be NULL).
  //
  // Note: make sure the implementation of this notification does not take
  // longer than the notification_interval argument of ci_start::create() to
  // avoid nested notifications. The first notification can be delayed with
  // the notify_delay argument.
  //
  class tenant_service_build_unloaded: public virtual tenant_service_base
  {
  public:
    virtual function<optional<string> (const string& tenant_id,
                                       const tenant_service&)>
    build_unloaded (const string& tenant_id,
                    tenant_service&&,
                    const diag_epilogue& log_writer) const noexcept = 0;
  };

  // Map of service type (tenant_service::type) to service.
  //
  using tenant_service_map = std::map<string, shared_ptr<tenant_service_base>>;

  // Every notification callback function that needs to produce any
  // diagnostics shall begin with:
  //
  // NOTIFICATION_DIAG (log_writer);
  //
  // This will instantiate the error, warn, info, and trace diagnostics
  // streams with the function's name.
  //
  // Note that a callback function is not expected to throw any exceptions.
  // This is, in particular, why this macro doesn't instantiate the fail
  // diagnostics stream.
  //
#define NOTIFICATION_DIAG(log_writer)                                   \
  const basic_mark error (severity::error,                              \
                          log_writer,                                   \
                          __PRETTY_FUNCTION__);                         \
  const basic_mark warn (severity::warning,                             \
                         log_writer,                                    \
                         __PRETTY_FUNCTION__);                          \
  const basic_mark info (severity::info,                                \
                         log_writer,                                    \
                         __PRETTY_FUNCTION__);                          \
  const basic_mark trace (severity::trace,                              \
                          log_writer,                                   \
                          __PRETTY_FUNCTION__)
}

#endif // MOD_TENANT_SERVICE_HXX
