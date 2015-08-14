// file      : brep/services.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <ap_config.h> // AP_MODULE_DECLARE_DATA

#include <web/apache/service>

#include <brep/package-search>
#include <brep/package-version-search>

using namespace brep;
using web::apache::service;

static package_search package_search_mod;
service AP_MODULE_DECLARE_DATA package_search_srv (
  "package-search",
  package_search_mod,
  {"db-host", "db-port", "conf"});

static package_version_search package_version_search_mod;
service AP_MODULE_DECLARE_DATA package_version_search_srv (
  "package-version-search",
  package_version_search_mod,
  {"db-host", "db-port", "conf"});
