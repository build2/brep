#! /usr/bin/env bash

odb -d pgsql --std c++11 --generate-query --generate-schema \
    --schema-format sql --schema-format embedded \
    --odb-epilogue '#include <brep/wrapper-traits>' \
    --hxx-prologue '#include <brep/wrapper-traits>' \
    --hxx-prologue '#include <brep/package-traits>' \
    -I .. -I ../../libbpkg -I ../../libbutl \
    --hxx-suffix "" --include-with-brackets \
    --include-prefix brep --guard-prefix BREP \
    package

xxd -i <package-extra.sql >package-extra
