// file      : mod/tenant-service.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_TENANT_SERVICE_HXX
#define MOD_TENANT_SERVICE_HXX

#include <map>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <libbrep/build.hxx>

namespace brep
{
  class tenant_service_base
  {
  public:
    virtual ~tenant_service_base () = default;
  };

  // Possible build notifications:
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
  // queued->building). As result, it is unlikely but possible to be notified
  // about the state transitions in the wrong order, especially if the
  // notifications take a long time. To minimize the chance of this happening,
  // the service implementation should strive to batch the queued state
  // notifications (or which there could be hundreds) in a single request if
  // at all possible. Also, if supported by the third-party API, it makes
  // sense for the implementation to protect against overwriting later states
  // with earlier. For example, if it's possible to place a condition on a
  // notification, it makes sense to only set the state to queued if none of
  // the later states (e.g., building) are already in effect.
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
  //
  class tenant_service_build_queued: public virtual tenant_service_base
  {
  public:
    // If the returned function is not NULL, it is called to update the
    // service data. It should return the new data or nullopt if no update is
    // necessary. Note: tenant_service::data passed to the callback and to the
    // returned function may not be the same. Also, the returned function may
    // be called multiple times (on transaction retries).
    //
    // The passed initial_state indicates the logical initial state and is
    // either absent, `building` (interrupted), or `built` (rebuild). Note
    // that all the passed build objects have the same initial state.
    //
    // The implementation of this and the below functions should normally not
    // need to make any decisions based on the passed build::state. Rather,
    // the function name suffix (_queued, _building, _built) signify the
    // logical end state.
    //
    virtual function<optional<string> (const tenant_service&)>
    build_queued (const tenant_service&,
                  const vector<build>&,
                  optional<build_state> initial_state) const = 0;
  };

  class tenant_service_build_building: public virtual tenant_service_base
  {
  public:
    virtual function<optional<string> (const tenant_service&)>
    build_building (const tenant_service&, const build&) const = 0;
  };

  class tenant_service_build_built: public virtual tenant_service_base
  {
  public:
    virtual function<optional<string> (const tenant_service&)>
    build_built (const tenant_service&, const build&) const = 0;
  };

  // Map of service type (tenant_service::type) to service.
  //
  using tenant_service_map = std::map<string, shared_ptr<tenant_service_base>>;
}

#endif // MOD_TENANT_SERVICE_HXX