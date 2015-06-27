DEBUG="-g -ggdb -fno-inline"

cd ./brep; cli --generate-file-scanner --suppress-usage --hxx-suffix "" \
	       --option-prefix "" ./options.cli; cd ..

cd ./brep; odb -d pgsql --std c++11 --generate-query --generate-schema \
               --odb-epilogue '#include <brep/wrapper-traits>' \
               --hxx-prologue '#include <brep/wrapper-traits>' \
	       -I .. -I ../../libbpkg -I ../../libbutl \
	       package; cd ..

g++ -shared $DEBUG -std=c++11 -I. -I/usr/include/apr-1 -I ../libbpkg \
     -I ../libbutl -L ../libbpkg/bpkg \
    -fPIC -o libbrep.so `find . -name '*.cxx'` -lbpkg -lodb-pgsql -lodb
