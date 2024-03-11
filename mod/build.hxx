// file      : mod/build.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_BUILD_HXX
#define MOD_BUILD_HXX

#include <odb/forward.hxx> // odb::core::connection_ptr

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <libbrep/build.hxx>
#include <libbrep/build-package.hxx>

#include <mod/diagnostics.hxx>
#include <mod/module-options.hxx>

// Various package build-related utilities.
//
namespace brep
{
  // Return the package build log url. By default the url is to the operations
  // combined log.
  //
  string
  build_log_url (const string& host, const dir_path& root,
                 const build&,
                 const string* operation = nullptr);

  // Return the package build forced rebuild url.
  //
  string
  build_force_url (const string& host, const dir_path& root, const build&);

  // Send the notification email for the specified package configuration
  // build. The build is expected to be in the built state.
  //
  void
  send_notification_email (const options::build_email_notification&,
                           const odb::core::connection_ptr&,
                           const build&,
                           const build_package&,
                           const build_package_config&,
                           const string& what,          // build, rebuild, etc.
                           const basic_mark& error,
                           const basic_mark* trace);
}

#endif // MOD_BUILD_HXX
