// file      : mod/services.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <ap_config.h> // AP_MODULE_DECLARE_DATA

#include <web/server/apache/service.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/mod-repository-root.hxx>

static brep::repository_root mod;
web::apache::service AP_MODULE_DECLARE_DATA brep_module ("brep", mod);
