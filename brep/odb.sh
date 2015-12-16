#! /usr/bin/env bash

odb -d pgsql --std c++11 --generate-query --generate-schema \
               --odb-epilogue '#include <brep/wrapper-traits>' \
               --hxx-prologue '#include <brep/wrapper-traits>' \
	       --hxx-prologue "#include <brep/package-traits>" \
	       --sql-epilogue-file package-extra.sql \
	       -I .. -I ../../libbpkg -I ../../libbutl \
               --hxx-suffix "" --include-with-brackets \
               --include-prefix brep --guard-prefix BREP \
	       package
