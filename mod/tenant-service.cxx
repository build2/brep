// file      : mod/tenant-service.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/tenant-service.hxx>

namespace brep
{
  void tenant_service_build_built::
  build_completed (const string& /* tenant_id */,
                   const tenant_service&,
                   const diag_epilogue& /* log_writer */) const noexcept
  {
    // If this notification is requested, then this function needs to be
    // overridden by the tenant service implementation.
    //
    assert (false);
  }
}
