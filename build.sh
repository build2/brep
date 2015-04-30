DEBUG="-g -ggdb -fno-inline"

cd ./brep; cli --generate-file-scanner --suppress-usage --hxx-suffix "" \
	       --option-prefix "" ./options.cli; cd ..

g++ -shared $DEBUG -std=c++11 -I. -I/usr/include/apr-1 \
    -fPIC -o libbrep.so `find . -name '*.cxx'`
