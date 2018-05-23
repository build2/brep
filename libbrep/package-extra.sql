-- This file should be parsable by the brep-migrate utility. To decrease the
-- parser complexity, the following restrictions are placed:
--
-- * comments must start with -- at the beginning of the line (ignoring
--   leading spaces)
-- * only CREATE and DROP statements for FUNCTION, TYPE and FOREIGN TABLE
-- * function bodies must be defined using $$-quoted strings
-- * strings other then function bodies must be quoted with ' or "
-- * statements must end with ";\n"
--

-- There is no need to drop to_tsvector() explicitly, as we can rely on "DROP
-- TYPE IF EXISTS weighted_text CASCADE" statement below, which will drop all
-- objects that depend on this type. Moreover this DROP FUNCTION statement will
-- fail for old versions of PostgreSQL (experienced for 9.2.14) with error:
-- type "weighted_text" does not exist.
--
-- DROP FUNCTION IF EXISTS to_tsvector(IN document weighted_text);
--
DROP FUNCTION IF EXISTS search_packages(IN query tsquery, INOUT name CITEXT);
DROP FUNCTION IF EXISTS search_latest_packages(IN query tsquery);
DROP FUNCTION IF EXISTS latest_package(INOUT name CITEXT);
DROP FUNCTION IF EXISTS latest_packages();

DROP TYPE IF EXISTS weighted_text CASCADE;
CREATE TYPE weighted_text AS (a TEXT, b TEXT, c TEXT, d TEXT);

-- Return the latest versions of internal packages as a set of package rows.
--
CREATE FUNCTION
latest_packages()
RETURNS SETOF package AS $$
  SELECT p1.*
  FROM package p1 LEFT JOIN package p2 ON (
    p1.internal_repository IS NOT NULL AND p1.name = p2.name AND
    p2.internal_repository IS NOT NULL AND
    (p1.version_epoch < p2.version_epoch OR
     p1.version_epoch = p2.version_epoch AND
     (p1.version_canonical_upstream < p2.version_canonical_upstream OR
      p1.version_canonical_upstream = p2.version_canonical_upstream AND
      (p1.version_canonical_release < p2.version_canonical_release OR
       p1.version_canonical_release = p2.version_canonical_release AND
       p1.version_revision < p2.version_revision))))
  WHERE
    p1.internal_repository IS NOT NULL AND p2.name IS NULL;
$$ LANGUAGE SQL STABLE;

-- Find the latest version of an internal package having the specified name.
-- Return a single row containing the package id, empty row set if the package
-- not found.
--
CREATE FUNCTION
latest_package(INOUT name CITEXT,
               OUT version_epoch INTEGER,
               OUT version_canonical_upstream TEXT,
               OUT version_canonical_release TEXT,
               OUT version_revision INTEGER)
RETURNS SETOF record AS $$
  SELECT name, version_epoch, version_canonical_upstream,
         version_canonical_release, version_revision
  FROM latest_packages()
  WHERE name = latest_package.name;
$$ LANGUAGE SQL STABLE;

-- Search for the latest version of an internal packages matching the specified
-- search query. Return a set of rows containing the package id and search
-- rank. If query is NULL, then match all packages and return 0 rank for
-- all rows.
--
CREATE FUNCTION
search_latest_packages(IN query tsquery,
                       OUT name CITEXT,
                       OUT version_epoch INTEGER,
                       OUT version_canonical_upstream TEXT,
                       OUT version_canonical_release TEXT,
                       OUT version_revision INTEGER,
                       OUT rank real)
RETURNS SETOF record AS $$
  SELECT name, version_epoch, version_canonical_upstream,
         version_canonical_release, version_revision,
         CASE
	   WHEN query IS NULL THEN 0
-- Weight mapping:           D     C    B    A
	   ELSE ts_rank_cd('{0.05, 0.2, 0.9, 1.0}', search_index, query)
	 END AS rank
  FROM latest_packages()
  WHERE query IS NULL OR search_index @@ query;
$$ LANGUAGE SQL STABLE;

-- Search for packages matching the search query and having the specified name.
-- Return a set of rows containing the package id and search rank. If query
-- is NULL, then match all packages and return 0 rank for all rows.
--
CREATE FUNCTION
search_packages(IN query tsquery,
                INOUT name CITEXT,
                OUT version_epoch INTEGER,
                OUT version_canonical_upstream TEXT,
                OUT version_canonical_release TEXT,
                OUT version_revision INTEGER,
                OUT rank real)
RETURNS SETOF record AS $$
  SELECT name, version_epoch, version_canonical_upstream,
         version_canonical_release, version_revision,
         CASE
	   WHEN query IS NULL THEN 0
-- Weight mapping:           D     C    B    A
	   ELSE ts_rank_cd('{0.05, 0.2, 0.9, 1.0}', search_index, query)
	 END AS rank
  FROM package
  WHERE
  internal_repository IS NOT NULL AND name = search_packages.name AND
  (query IS NULL OR search_index @@ query);
$$ LANGUAGE SQL STABLE;

-- Parse weighted_text to tsvector.
--
CREATE FUNCTION
to_tsvector(IN document weighted_text)
RETURNS tsvector AS $$
  SELECT
    CASE
      WHEN document IS NULL THEN NULL
      ELSE
        setweight(to_tsvector(document.a), 'A') ||
	setweight(to_tsvector(document.b), 'B') ||
	setweight(to_tsvector(document.c), 'C') ||
	setweight(to_tsvector(document.d), 'D')
    END
$$ LANGUAGE SQL IMMUTABLE;
