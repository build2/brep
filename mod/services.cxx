// file      : mod/services.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <ap_config.h> // AP_MODULE_DECLARE_DATA

#include <web/apache/service.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/mod-repository-root.hxx>

static brep::repository_root mod;
web::apache::service AP_MODULE_DECLARE_DATA brep_module ("brep", mod);
