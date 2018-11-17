// file      : mod/mod-build-configs.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-build-configs.hxx>

#include <libstudxml/serializer.hxx>

#include <web/xhtml.hxx>
#include <web/module.hxx>

#include <mod/page.hxx>
#include <mod/options.hxx>
#include <mod/build-config.hxx>

using namespace std;
using namespace bbot;
using namespace brep::cli;

// While currently the user-defined copy constructor is not required (we don't
// need to deep copy nullptr's), it is a good idea to keep the placeholder
// ready for less trivial cases.
//
brep::build_configs::
build_configs (const build_configs& r)
    : handler (r),
      options_ (r.initialized_ ? r.options_ : nullptr),
      build_conf_ (r.initialized_ ? r.build_conf_ : nullptr)
{
}

void brep::build_configs::
init (scanner& s)
{
  HANDLER_DIAG;

  options_ = make_shared<options::build_configs> (
    s, unknown_mode::fail, unknown_mode::fail);

  if (options_->build_config_specified ())
  try
  {
    build_conf_ = shared_build_config (options_->build_config ());
  }
  catch (const io_error& e)
  {
    fail << "unable to read build configuration '"
         << options_->build_config () << "': " << e;
  }
}

bool brep::build_configs::
handle (request& rq, response& rs)
{
  using namespace web::xhtml;

  HANDLER_DIAG;

  if (build_conf_ == nullptr)
    throw invalid_request (501, "not implemented");

  const dir_path& root (options_->root ());

  // Make sure no parameters passed.
  //
  try
  {
    name_value_scanner s (rq.parameters (1024));
    params::build_configs (s, unknown_mode::fail, unknown_mode::fail);
  }
  catch (const cli::exception& e)
  {
    throw invalid_request (400, e.what ());
  }

  static const string title ("Build Configurations");
  xml::serializer s (rs.content (), title);

  s << HTML
    <<   HEAD
    <<     TITLE << title << ~TITLE
    <<     CSS_LINKS (path ("build-configs.css"), root)
    <<   ~HEAD
    <<   BODY
    <<     DIV_HEADER (options_->logo (), options_->menu (), root, tenant)
    <<     DIV(ID="content")
    <<       TABLE(CLASS="proplist")
    <<         TBODY;

  for (const build_config& c: *build_conf_)
  {
    if (belongs (c, "all"))
      s << TR(CLASS="config")
        <<   TD << SPAN(CLASS="value") << c.name << ~SPAN << ~TD
        << ~TR;
  }

  s <<         ~TBODY
    <<       ~TABLE
    <<     ~DIV
    <<   ~BODY
    << ~HTML;

  return true;
}
