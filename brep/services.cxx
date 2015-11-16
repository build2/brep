// file      : brep/services.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <ap_config.h> // AP_MODULE_DECLARE_DATA

#include <web/apache/service>

#include <brep/package-search>
#include <brep/package-details>
#include <brep/repository-details>
#include <brep/package-version-details>

using namespace brep;
using web::apache::service;

static package_search package_search_mod;
service AP_MODULE_DECLARE_DATA package_search_srv (
  "package-search",
  package_search_mod,
  {"root", "db-host", "db-port", "conf"});

static package_details package_details_mod;
service AP_MODULE_DECLARE_DATA package_details_srv (
  "package-details",
  package_details_mod,
  {"root", "db-host", "db-port", "conf"});

static package_version_details package_version_details_mod;
service AP_MODULE_DECLARE_DATA package_version_details_srv (
  "package-version-details",
  package_version_details_mod,
  {"root", "db-host", "db-port", "conf"});

static repository_details repository_details_mod;
service AP_MODULE_DECLARE_DATA repository_details_srv (
  "repository-details",
  repository_details_mod,
  {"root", "db-host", "db-port", "conf"});
