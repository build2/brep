// file      : services.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <brep/view>
#include <brep/search>

#include <web/apache/service>

using namespace brep;
using web::apache::service;

static search search_mod;
service AP_MODULE_DECLARE_DATA search_srv ("search",
                                           search_mod,
                                           {"db-host", "db-port", "conf"});

static view view_mod;
service AP_MODULE_DECLARE_DATA view_srv ("view",
                                         view_mod,
                                         {"db-host", "db-port", "conf"});
