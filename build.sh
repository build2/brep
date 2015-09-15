DEBUG="-g -ggdb -fno-inline"

cd ./brep

echo "odb package"

odb -d pgsql --std c++11 --generate-query --generate-schema \
               --odb-epilogue '#include <brep/wrapper-traits>' \
               --hxx-prologue '#include <brep/wrapper-traits>' \
	       -I .. -I ../../libbpkg -I ../../libbutl \
               --hxx-suffix "" --include-with-brackets \
               --include-prefix brep --guard-prefix BREP \
	       package
e=$?
if test $e -ne 0; then exit $e; fi

echo "g++ libbrep.so"

s="package.cxx package-odb.cxx"

g++ -shared $DEBUG -std=c++11 -I.. -I../../libbpkg \
    -I../../libbutl -L../../libbpkg/bpkg -L../../libbutl/butl \
    -fPIC -o libbrep.so $s -lbpkg -lbutl -lodb-pgsql -lodb

echo "cli brep-apache options"

cli --include-with-brackets --include-prefix brep  --hxx-suffix "" \
    --guard-prefix BREP --generate-file-scanner --suppress-usage \
    --option-prefix "" ./options.cli

echo "g++ libbrep-apache.so"

s="package-search.cxx package-version-search.cxx package-version-details.cxx \
module.cxx diagnostics.cxx page.cxx services.cxx options.cxx \
shared-database.cxx \
../web/apache/request.cxx ../web/apache/service.cxx \
../web/mime-url-encoding.cxx"

g++ -shared $DEBUG -std=c++11 -I. -I/usr/include/apr-1 -I/usr/include/httpd \
    -I.. -I../../libbpkg -I../../libbutl -L. -L../../libbpkg/bpkg \
    -fPIC -o libbrep-apache.so $s -lbrep -lbpkg -lodb-pgsql -lodb -lstudxml

cd ../loader

echo "cli loader options"

cli --hxx-suffix "" ./options.cli

echo "g++ brep-loader"

s="loader.cxx options.cxx"

g++ $DEBUG -std=c++11 -I. -I.. -I../../libbpkg \
    -I../../libbutl -L../brep -L../../libbpkg/bpkg -L../../libbutl/butl \
    -o brep-loader $s -lbrep -lbpkg -lbutl -lodb-pgsql -lodb

cd ../tests/loader

echo "g++ tests/loader"

s="driver.cxx"

g++ $DEBUG -std=c++11 -I. -I../.. -I../../../libbpkg \
    -I../../../libbutl -L../../brep -L../../../libbpkg/bpkg \
    -L../../../libbutl/butl \
    -o driver $s -lbrep -lbpkg -lbutl -lodb-pgsql -lodb
