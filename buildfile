# file      : buildfile
# copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
# license   : MIT; see accompanying LICENSE file

d = brep/ etc/ load/ migrate/ tests/ www/ doc/
./: $d doc{INSTALL INSTALL-DEV LICENSE README version} file{manifest}
include $d

doc{INSTALL*}: install = false
