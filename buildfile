# file      : buildfile
# copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
# license   : MIT; see accompanying LICENSE file

d = brep/ mod/ load/ migrate/ tests/ www/ doc/ etc/
./: $d doc{INSTALL INSTALL-DEV LICENSE NEWS README version} file{manifest}
include $d

doc{INSTALL*}: install = false
