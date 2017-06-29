-- This file should be parsable by the brep-migrate utility. To decrease the
-- parser complexity, there is a number of restrictions, see package-extra.sql
-- file for details.
--

-- The foreign table for build_package object.
--
--
DROP FOREIGN TABLE IF EXISTS build_package;

CREATE FOREIGN TABLE build_package (
  name TEXT NOT NULL,
  version_epoch INTEGER NOT NULL,
  version_canonical_upstream TEXT NOT NULL,
  version_canonical_release TEXT NOT NULL COLLATE "C",
  version_revision INTEGER NOT NULL,
  version_upstream TEXT NOT NULL,
  version_release TEXT NULL,
  internal_repository TEXT NULL)
SERVER package_server OPTIONS (table_name 'package');
