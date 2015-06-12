// file      : brep/package.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <brep/package>

#include <odb/database.hxx>

#include <brep/package-odb.hxx>

namespace brep
{
  // package_version
  //
  void package_version::
  id (const package_version_id& v, odb::database& db)
  {
    version = v.version;
    package = db.load<package_type> (v.package);
  }
}
