/* This file was generated by ODB, object-relational mapping (ORM)
 * compiler for C++.
 */

ALTER TABLE "package"
  ADD COLUMN "reviews_pass" BIGINT NULL,
  ADD COLUMN "reviews_fail" BIGINT NULL,
  ADD COLUMN "reviews_manifest_file" TEXT NULL;

UPDATE "schema_version"
  SET "version" = 35, "migration" = TRUE
  WHERE "name" = 'package';

