#! /usr/bin/env bash

trap 'exit 1' ERR

odb=odb
lib="\
-I$HOME/work/odb/libodb-sqlite-default \
-I$HOME/work/odb/libodb-sqlite \
-I$HOME/work/odb/libodb-default \
-I$HOME/work/odb/libodb"

$odb $lib -d pgsql --std c++11 --generate-query --generate-schema \
    --schema-format sql --schema-format embedded \
    --odb-epilogue '#include <brep/wrapper-traits>' \
    --hxx-prologue '#include <brep/wrapper-traits>' \
    --hxx-prologue '#include <brep/package-traits>' \
    -I .. -I ../../libbpkg -I ../../libbutl \
    --hxx-suffix "" --include-with-brackets \
    --include-prefix brep --guard-prefix BREP \
    package

xxd -i <package-extra.sql >package-extra
