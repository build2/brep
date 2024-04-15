#! /usr/bin/env bash

trap 'exit 1' ERR

odb=odb
inc=()

if test -d ../.bdep; then

  if [ -n "$1" ]; then
    cfg="$1"
  else
    # Use default configuration for headers.
    #
    cfg="$(bdep config list -d .. | \
sed -r -ne 's#^(@[^ ]+ )?([^ ]+)/ .*default.*$#\2#p')"
  fi

  inc+=("-I$(echo "$cfg"/libodb-[1-9]*/)")
  inc+=("-I$(echo "$cfg"/libodb-pgsql-[1-9]*/)")

  inc+=("-I$cfg/libbutl")
  inc+=("-I../../libbutl")

  inc+=("-I$cfg/libbpkg")
  inc+=("-I../../libbpkg")

  inc+=("-I$cfg/libbbot")
  inc+=("-I../../libbbot")

  inc+=("-I$cfg/brep")
  inc+=("-I..")

else

  inc+=("-I$HOME/work/odb/builds/default/libodb-pgsql-default")
  inc+=("-I$HOME/work/odb/libodb-pgsql")

  inc+=("-I$HOME/work/odb/builds/default/libodb-default")
  inc+=("-I$HOME/work/odb/libodb")

  inc+=(-I.. -I../../libbbot -I../../libbpkg -I../../libbutl)

fi

rm -f {package,build}-???-{pre,post}.sql

$odb "${inc[@]}" -d pgsql --std c++14 --generate-query       \
    --odb-epilogue '#include <libbutl/small-vector-odb.hxx>' \
    --odb-epilogue '#include <libbrep/wrapper-traits.hxx>'   \
    --hxx-prologue '#include <libbutl/small-vector-odb.hxx>' \
    --hxx-prologue '#include <libbrep/wrapper-traits.hxx>'   \
    --hxx-prologue '#include <libbrep/common-traits.hxx>'    \
    -DLIBODB_BUILD2 -DLIBODB_PGSQL_BUILD2                    \
    --include-with-brackets --include-prefix libbrep         \
    --guard-prefix LIBBREP                                   \
    common.hxx

$odb "${inc[@]}" -d pgsql --std c++14 --generate-query --generate-schema \
    --schema-format sql --schema-format embedded --schema-name package   \
    --odb-epilogue '#include <libbutl/small-vector-odb.hxx>'             \
    --odb-epilogue '#include <libbrep/wrapper-traits.hxx>'               \
    --hxx-prologue '#include <libbrep/package-traits.hxx>'               \
    --generate-prepared -DLIBODB_BUILD2 -DLIBODB_PGSQL_BUILD2            \
    --include-with-brackets --include-prefix libbrep                     \
    --guard-prefix LIBBREP                                               \
    package.hxx

xxd -i <package-extra.sql >package-extra.hxx

$odb "${inc[@]}" -d pgsql --std c++14 --generate-query --generate-schema \
    --schema-format sql --schema-format embedded --schema-name build     \
    --odb-epilogue '#include <libbutl/small-vector-odb.hxx>'             \
    --odb-epilogue '#include <libbrep/wrapper-traits.hxx>'               \
    --generate-prepared -DLIBODB_BUILD2 -DLIBODB_PGSQL_BUILD2            \
    --include-with-brackets --include-prefix libbrep                     \
    --guard-prefix LIBBREP                                               \
    build.hxx

$odb "${inc[@]}" -d pgsql --std c++14 --generate-query        \
    --odb-epilogue '#include <libbutl/small-vector-odb.hxx>'  \
    --odb-epilogue '#include <libbrep/wrapper-traits.hxx>'    \
    --generate-prepared -DLIBODB_BUILD2 -DLIBODB_PGSQL_BUILD2 \
    --include-with-brackets --include-prefix libbrep          \
    --guard-prefix LIBBREP                                    \
    build-package.hxx

xxd -i <build-extra.sql >build-extra.hxx
