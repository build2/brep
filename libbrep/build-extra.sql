-- This file should be parsable by the brep-migrate utility. To decrease the
-- parser complexity, there is a number of restrictions, see package-extra.sql
-- file for details.
--

DROP FOREIGN TABLE IF EXISTS build_package_constraints;

DROP FOREIGN TABLE IF EXISTS build_package;

DROP FOREIGN TABLE IF EXISTS build_repository;

-- The foreign table for build_repository object.
--
--
CREATE FOREIGN TABLE build_repository (
  name TEXT NOT NULL,
  location TEXT NOT NULL,
  certificate_fingerprint TEXT NULL)
SERVER package_server OPTIONS (table_name 'repository');

-- The foreign table for build_package object.
--
--
CREATE FOREIGN TABLE build_package (
  name CITEXT NOT NULL,
  version_epoch INTEGER NOT NULL,
  version_canonical_upstream TEXT NOT NULL,
  version_canonical_release TEXT NOT NULL COLLATE "C",
  version_revision INTEGER NOT NULL,
  version_upstream TEXT NOT NULL,
  version_release TEXT NULL,
  internal_repository TEXT NULL)
SERVER package_server OPTIONS (table_name 'package');

-- The foreign table for the build_package object constraints member (that is
-- of a container type).
--
--
CREATE FOREIGN TABLE build_package_constraints (
  name CITEXT NOT NULL,
  version_epoch INTEGER NOT NULL,
  version_canonical_upstream TEXT NOT NULL,
  version_canonical_release TEXT NOT NULL COLLATE "C",
  version_revision INTEGER NOT NULL,
  index BIGINT NOT NULL,
  exclusion BOOLEAN NOT NULL,
  config TEXT NOT NULL,
  target TEXT NULL)
SERVER package_server OPTIONS (table_name 'package_build_constraints');
