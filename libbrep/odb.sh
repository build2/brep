#! /usr/bin/env bash

trap 'exit 1' ERR

odb=odb
lib="\
-I$HOME/work/odb/libodb-pgsql-default \
-I$HOME/work/odb/libodb-pgsql \
-I$HOME/work/odb/libodb-default \
-I$HOME/work/odb/libodb"

$odb $lib -d pgsql --std c++11 --generate-query              \
    --odb-epilogue '#include <libbrep/wrapper-traits.hxx>'   \
    --hxx-prologue '#include <libbrep/wrapper-traits.hxx>'   \
    -DLIBODB_BUILD2 -DLIBODB_PGSQL_BUILD2                    \
    -I .. -I ../../libbbot -I ../../libbpkg -I ../../libbutl \
    --hxx-suffix ".hxx" --include-with-brackets              \
    --include-prefix libbrep --guard-prefix LIBBREP          \
    common.hxx

$odb $lib -d pgsql --std c++11 --generate-query --generate-schema      \
    --schema-format sql --schema-format embedded --schema-name package \
    --odb-epilogue '#include <libbrep/wrapper-traits.hxx>'             \
    --hxx-prologue '#include <libbrep/package-traits.hxx>'             \
    --generate-prepared -DLIBODB_BUILD2 -DLIBODB_PGSQL_BUILD2          \
    -I .. -I ../../libbbot -I ../../libbpkg -I ../../libbutl           \
    --hxx-suffix ".hxx" --include-with-brackets                        \
    --include-prefix libbrep --guard-prefix LIBBREP                    \
    package.hxx

xxd -i <package-extra.sql >package-extra.hxx

$odb $lib -d pgsql --std c++11 --generate-query --generate-schema    \
    --schema-format sql --schema-format embedded --schema-name build \
    --odb-epilogue '#include <libbrep/wrapper-traits.hxx>'           \
    --generate-prepared -DLIBODB_BUILD2 -DLIBODB_PGSQL_BUILD2        \
    -I .. -I ../../libbbot -I ../../libbpkg -I ../../libbutl         \
    --hxx-suffix ".hxx" --include-with-brackets                      \
    --include-prefix libbrep --guard-prefix LIBBREP                  \
    build.hxx
