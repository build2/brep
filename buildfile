# file      : buildfile
# copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
# license   : MIT; see accompanying LICENSE file

d = brep/ etc/ loader/ tests/ www/
./: $d doc{INSTALL INSTALL-DEV LICENSE version} file{manifest}
include $d

$src_base/doc{INSTALL INSTALL-DEV}: install = false
