// file      : brep/services.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <ap_config.h> // AP_MODULE_DECLARE_DATA

#include <web/apache/service>

#include <brep/repository-root>

static brep::repository_root mod;
web::apache::service AP_MODULE_DECLARE_DATA brep_module ("brep", mod);
