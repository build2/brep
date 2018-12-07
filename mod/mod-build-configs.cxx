// file      : mod/mod-build-configs.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <mod/mod-build-configs.hxx>

#include <libstudxml/serializer.hxx>

#include <web/xhtml.hxx>
#include <web/module.hxx>

#include <mod/page.hxx>
#include <mod/options.hxx>

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
      build_config_module (r),
      options_ (r.initialized_ ? r.options_ : nullptr)
{
}

void brep::build_configs::
init (scanner& s)
{
  HANDLER_DIAG;

  options_ = make_shared<options::build_configs> (
    s, unknown_mode::fail, unknown_mode::fail);

  if (options_->build_config_specified ())
    build_config_module::init (static_cast<options::build> (*options_));
}

bool brep::build_configs::
handle (request& rq, response& rs)
{
  using namespace web::xhtml;

  HANDLER_DIAG;

  if (build_conf_ == nullptr)
    throw invalid_request (501, "not implemented");

  const size_t page_configs (options_->build_config_page_entries ());
  const dir_path& root (options_->root ());

  params::build_configs params;

  try
  {
    name_value_scanner s (rq.parameters (1024));
    params = params::build_configs (s, unknown_mode::fail, unknown_mode::fail);
  }
  catch (const cli::exception& e)
  {
    throw invalid_request (400, e.what ());
  }

  size_t page (params.page ());

  const char* title ("Build Configurations");
  xml::serializer s (rs.content (), title);

  s << HTML
    <<   HEAD
    <<     TITLE << title << ~TITLE
    <<     CSS_LINKS (path ("build-configs.css"), root)
    <<   ~HEAD
    <<   BODY
    <<     DIV_HEADER (options_->logo (), options_->menu (), root, tenant)
    <<     DIV(ID="content");

  // Print build configurations that belong to the 'all' class.
  //
  // We will calculate the total configuration count and cache configurations
  // for printing (skipping an appropriate number of them for page number
  // greater than one) on the same pass. Note that we need to print the count
  // before printing the configurations.
  //
  size_t count (0);
  vector<const build_config*> configs;
  configs.reserve (page_configs);

  size_t skip (page * page_configs);
  size_t print (page_configs);
  for (const build_config& c: *build_conf_)
  {
    if (belongs (c, "all"))
    {
      if (skip != 0)
        --skip;
      else if (print != 0)
      {
        configs.emplace_back (&c);
        --print;
      }

      ++count;
    }
  }

  // Print configuration count.
  //
  s << DIV_COUNTER (count, "Build Configuration", title);

  // Finally, print the cached build configurations.
  //
  // Enclose the subsequent tables to be able to use nth-child CSS selector.
  //
  s << DIV;
  for (const build_config* c: configs)
  {
    string classes;
    for (const string& cls: c->classes)
    {
      if (!classes.empty ())
        classes += ' ';

      classes += cls;

      // Append the base class, if present.
      //
      auto i (build_conf_->class_inheritance_map.find (cls));
      if (i != build_conf_->class_inheritance_map.end ())
      {
        classes += ':';
        classes += i->second;
      }
    }

    s << TABLE(CLASS="proplist config")
      <<   TBODY
      <<     TR_VALUE ("name", c->name)
      <<     TR_VALUE ("target", c->target.string ())
      <<     TR_VALUE ("classes", classes)
      <<   ~TBODY
      << ~TABLE;
  }
  s << ~DIV;

  s <<       DIV_PAGER (page,
                        count,
                        page_configs,
                        options_->build_config_pages (),
                        root.string () + "?build-configs")
    <<     ~DIV
    <<   ~BODY
    << ~HTML;

  return true;
}
