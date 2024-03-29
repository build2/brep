-- This file should be parsable by the brep-migrate utility. To decrease the
-- parser complexity, there is a number of restrictions, see package-extra.sql
-- file for details.
--
-- Increment the database 'build' schema version when update this file, see
-- package-extra.sql file for details.
--

DROP FOREIGN TABLE IF EXISTS build_package_config_auxiliaries;

DROP FOREIGN TABLE IF EXISTS build_package_config_constraints;

DROP FOREIGN TABLE IF EXISTS build_package_config_builds;

DROP FOREIGN TABLE IF EXISTS build_package_configs;

DROP FOREIGN TABLE IF EXISTS build_package_auxiliaries;

DROP FOREIGN TABLE IF EXISTS build_package_constraints;

DROP FOREIGN TABLE IF EXISTS build_package_builds;

DROP FOREIGN TABLE IF EXISTS build_package_tests;

DROP FOREIGN TABLE IF EXISTS build_package_requirement_alternative_requirements;

DROP FOREIGN TABLE IF EXISTS build_package_requirement_alternatives;

DROP FOREIGN TABLE IF EXISTS build_package_requirements;

DROP FOREIGN TABLE IF EXISTS build_package;

DROP FOREIGN TABLE IF EXISTS build_repository;

DROP FOREIGN TABLE IF EXISTS build_tenant;

-- The foreign table for build_tenant object.
--
CREATE FOREIGN TABLE build_tenant (
  id TEXT NOT NULL,
  private BOOLEAN NOT NULL,
  interactive TEXT NULL,
  archived BOOLEAN NOT NULL,
  service_id TEXT NULL,
  service_type TEXT NULL,
  service_data TEXT NULL,
  queued_timestamp BIGINT NULL,
  toolchain_name TEXT OPTIONS (column_name 'build_toolchain_name') NULL,
  toolchain_version_epoch INTEGER OPTIONS (column_name 'build_toolchain_version_epoch') NULL,
  toolchain_version_canonical_upstream TEXT OPTIONS (column_name 'build_toolchain_version_canonical_upstream') NULL,
  toolchain_version_canonical_release TEXT OPTIONS (column_name 'build_toolchain_version_canonical_release') NULL,
  toolchain_version_revision INTEGER OPTIONS (column_name 'build_toolchain_version_revision') NULL,
  toolchain_version_upstream TEXT OPTIONS (column_name 'build_toolchain_version_upstream') NULL,
  toolchain_version_release TEXT OPTIONS (column_name 'build_toolchain_version_release') NULL)
SERVER package_server OPTIONS (table_name 'tenant');

-- The foreign table for build_repository object.
--
CREATE FOREIGN TABLE build_repository (
  tenant TEXT NOT NULL,
  canonical_name TEXT NOT NULL,
  location_url TEXT NOT NULL,
  location_type TEXT NOT NULL,
  certificate_fingerprint TEXT NULL)
SERVER package_server OPTIONS (table_name 'repository');

-- The foreign table for build_package object.
--
CREATE FOREIGN TABLE build_package (
  tenant TEXT NOT NULL,
  name CITEXT NOT NULL,
  version_epoch INTEGER NOT NULL,
  version_canonical_upstream TEXT NOT NULL,
  version_canonical_release TEXT NOT NULL COLLATE "C",
  version_revision INTEGER NOT NULL,
  version_upstream TEXT NOT NULL,
  version_release TEXT NULL,
  project CITEXT NOT NULL,
  build_email TEXT NULL,
  build_email_comment TEXT NULL,
  build_warning_email TEXT NULL,
  build_warning_email_comment TEXT NULL,
  build_error_email TEXT NULL,
  build_error_email_comment TEXT NULL,
  internal_repository_tenant TEXT NULL,
  internal_repository_canonical_name TEXT NULL,
  buildable BOOLEAN NOT NULL)
SERVER package_server OPTIONS (table_name 'package');

-- The foreign tables for the build_package object requirements member (that
-- is of a 3-dimensional container type).
--
CREATE FOREIGN TABLE build_package_requirements (
  tenant TEXT NOT NULL,
  name CITEXT NOT NULL,
  version_epoch INTEGER NOT NULL,
  version_canonical_upstream TEXT NOT NULL,
  version_canonical_release TEXT NOT NULL COLLATE "C",
  version_revision INTEGER NOT NULL,
  index BIGINT NOT NULL,
  buildtime BOOLEAN NOT NULL,
  comment TEXT NOT NULL)
SERVER package_server OPTIONS (table_name 'package_requirements');

CREATE FOREIGN TABLE build_package_requirement_alternatives (
  tenant TEXT NOT NULL,
  name CITEXT NOT NULL,
  version_epoch INTEGER NOT NULL,
  version_canonical_upstream TEXT NOT NULL,
  version_canonical_release TEXT NOT NULL COLLATE "C",
  version_revision INTEGER NOT NULL,
  requirement_index BIGINT NOT NULL,
  index BIGINT NOT NULL,
  enable TEXT NULL,
  reflect TEXT NULL)
SERVER package_server OPTIONS (table_name 'package_requirement_alternatives');

CREATE FOREIGN TABLE build_package_requirement_alternative_requirements (
  tenant TEXT NOT NULL,
  name CITEXT NOT NULL,
  version_epoch INTEGER NOT NULL,
  version_canonical_upstream TEXT NOT NULL,
  version_canonical_release TEXT NOT NULL COLLATE "C",
  version_revision INTEGER NOT NULL,
  requirement_index BIGINT NOT NULL,
  alternative_index BIGINT NOT NULL,
  index BIGINT NOT NULL,
  id TEXT NOT NULL)
SERVER package_server OPTIONS (table_name 'package_requirement_alternative_requirements');

-- The foreign table for the build_package object tests member (that is of a
-- container type).
--
CREATE FOREIGN TABLE build_package_tests (
  tenant TEXT NOT NULL,
  name CITEXT NOT NULL,
  version_epoch INTEGER NOT NULL,
  version_canonical_upstream TEXT NOT NULL,
  version_canonical_release TEXT NOT NULL COLLATE "C",
  version_revision INTEGER NOT NULL,
  index BIGINT NOT NULL,
  test_name CITEXT NOT NULL,
  test_min_version_epoch INTEGER NULL,
  test_min_version_canonical_upstream TEXT NULL,
  test_min_version_canonical_release TEXT NULL,
  test_min_version_revision INTEGER NULL,
  test_min_version_upstream TEXT NULL,
  test_min_version_release TEXT NULL,
  test_max_version_epoch INTEGER NULL,
  test_max_version_canonical_upstream TEXT NULL,
  test_max_version_canonical_release TEXT NULL,
  test_max_version_revision INTEGER NULL,
  test_max_version_upstream TEXT NULL,
  test_max_version_release TEXT NULL,
  test_min_open BOOLEAN NULL,
  test_max_open BOOLEAN NULL,
  test_package_tenant TEXT NULL,
  test_package_name CITEXT NULL,
  test_package_version_epoch INTEGER NULL,
  test_package_version_canonical_upstream TEXT NULL,
  test_package_version_canonical_release TEXT NULL COLLATE "C",
  test_package_version_revision INTEGER NULL,
  test_type TEXT NOT NULL,
  test_buildtime BOOLEAN NOT NULL,
  test_enable TEXT NULL,
  test_reflect TEXT NULL)
SERVER package_server OPTIONS (table_name 'package_tests');

-- The foreign table for the build_package object builds member (that is of a
-- container type).
--
CREATE FOREIGN TABLE build_package_builds (
  tenant TEXT NOT NULL,
  name CITEXT NOT NULL,
  version_epoch INTEGER NOT NULL,
  version_canonical_upstream TEXT NOT NULL,
  version_canonical_release TEXT NOT NULL COLLATE "C",
  version_revision INTEGER NOT NULL,
  index BIGINT NOT NULL,
  expression TEXT NOT NULL,
  comment TEXT NOT NULL)
SERVER package_server OPTIONS (table_name 'package_builds');

-- The foreign table for the build_package object constraints member (that is
-- of a container type).
--
CREATE FOREIGN TABLE build_package_constraints (
  tenant TEXT NOT NULL,
  name CITEXT NOT NULL,
  version_epoch INTEGER NOT NULL,
  version_canonical_upstream TEXT NOT NULL,
  version_canonical_release TEXT NOT NULL COLLATE "C",
  version_revision INTEGER NOT NULL,
  index BIGINT NOT NULL,
  exclusion BOOLEAN NOT NULL,
  config TEXT NOT NULL,
  target TEXT NULL,
  comment TEXT NOT NULL)
SERVER package_server OPTIONS (table_name 'package_build_constraints');

-- The foreign table for the build_package object auxiliaries member (that is
-- of a container type).
--
CREATE FOREIGN TABLE build_package_auxiliaries (
  tenant TEXT NOT NULL,
  name CITEXT NOT NULL,
  version_epoch INTEGER NOT NULL,
  version_canonical_upstream TEXT NOT NULL,
  version_canonical_release TEXT NOT NULL COLLATE "C",
  version_revision INTEGER NOT NULL,
  index BIGINT NOT NULL,
  environment_name TEXT NOT NULL,
  config TEXT NOT NULL,
  comment TEXT NOT NULL)
SERVER package_server OPTIONS (table_name 'package_build_auxiliaries');

-- The foreign tables for the build_package object configs member (that is a
-- container of values containing containers.
--
CREATE FOREIGN TABLE build_package_configs (
  tenant TEXT NOT NULL,
  name CITEXT NOT NULL,
  version_epoch INTEGER NOT NULL,
  version_canonical_upstream TEXT NOT NULL,
  version_canonical_release TEXT NOT NULL COLLATE "C",
  version_revision INTEGER NOT NULL,
  index BIGINT NOT NULL,
  config_name TEXT NOT NULL,
  config_arguments TEXT NULL,
  config_comment TEXT NOT NULL,
  config_email TEXT NULL,
  config_email_comment TEXT NULL,
  config_warning_email TEXT NULL,
  config_warning_email_comment TEXT NULL,
  config_error_email TEXT NULL,
  config_error_email_comment TEXT NULL)
SERVER package_server OPTIONS (table_name 'package_build_configs');

CREATE FOREIGN TABLE build_package_config_builds (
  tenant TEXT NOT NULL,
  name CITEXT NOT NULL,
  version_epoch INTEGER NOT NULL,
  version_canonical_upstream TEXT NOT NULL,
  version_canonical_release TEXT NOT NULL COLLATE "C",
  version_revision INTEGER NOT NULL,
  config_index BIGINT NOT NULL,
  index BIGINT NOT NULL,
  expression TEXT NOT NULL,
  comment TEXT NOT NULL)
SERVER package_server OPTIONS (table_name 'package_build_config_builds');

CREATE FOREIGN TABLE build_package_config_constraints (
  tenant TEXT NOT NULL,
  name CITEXT NOT NULL,
  version_epoch INTEGER NOT NULL,
  version_canonical_upstream TEXT NOT NULL,
  version_canonical_release TEXT NOT NULL COLLATE "C",
  version_revision INTEGER NOT NULL,
  config_index BIGINT NOT NULL,
  index BIGINT NOT NULL,
  exclusion BOOLEAN NOT NULL,
  config TEXT NOT NULL,
  target TEXT NULL,
  comment TEXT NOT NULL)
SERVER package_server OPTIONS (table_name 'package_build_config_constraints');

CREATE FOREIGN TABLE build_package_config_auxiliaries (
  tenant TEXT NOT NULL,
  name CITEXT NOT NULL,
  version_epoch INTEGER NOT NULL,
  version_canonical_upstream TEXT NOT NULL,
  version_canonical_release TEXT NOT NULL COLLATE "C",
  version_revision INTEGER NOT NULL,
  config_index BIGINT NOT NULL,
  index BIGINT NOT NULL,
  environment_name TEXT NOT NULL,
  config TEXT NOT NULL,
  comment TEXT NOT NULL)
SERVER package_server OPTIONS (table_name 'package_build_config_auxiliaries');
