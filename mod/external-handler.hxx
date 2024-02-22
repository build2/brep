// file      : mod/external-handler.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_EXTERNAL_HANDLER_HXX
#define MOD_EXTERNAL_HANDLER_HXX

#include <libbutl/manifest-types.hxx>

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

#include <mod/diagnostics.hxx>

namespace brep
{
  // Utility for running external handler programs.
  //
  namespace external_handler
  {
    // Run an external handler program and, if it exited normally with the
    // zero exit status, return the result manifest it is expected to write to
    // stdout, containing at least the HTTP status value. Otherwise, log an
    // error and return nullopt. Redirect the program stderr to the web server
    // error log.
    //
    // If the timeout (in seconds) is not zero and the handler program does
    // not exit in the allotted time, then it is killed and its termination is
    // treated as abnormal.
    //
    // Note that warnings can be logged regardless of the program success. If
    // the trace argument is not NULL, then trace records are also logged.
    //
    struct result_manifest
    {
      uint16_t status;

      // All values, including status but excluding format version and
      // end-of-manifest.
      //
      vector<butl::manifest_name_value> values;
    };

    optional<result_manifest>
    run (const path& handler,
         const strings& args,
         const dir_path& data_dir,
         size_t timeout,
         const basic_mark& error,
         const basic_mark& warn,
         const basic_mark* trace);
  }
}

#endif // MOD_EXTERNAL_HANDLER_HXX
