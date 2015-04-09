// file      : services.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <web/apache/service>

#include <brep/search>
#include <brep/view>

using namespace brep;
using web::apache::service;

static const search search_mod;
service<search> AP_MODULE_DECLARE_DATA search_srv ("search", search_mod);

static const view view_mod;
service<view> AP_MODULE_DECLARE_DATA view_srv ("view", view_mod);
