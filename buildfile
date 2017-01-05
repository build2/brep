# file      : buildfile
# copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
# license   : MIT; see accompanying LICENSE file

d = brep/ mod/ load/ migrate/ tests/ www/ doc/ etc/
./: $d doc{INSTALL INSTALL-DEV LICENSE NEWS README version} file{manifest}
include $d

# Don't install tests or the INSTALL* files.
#
dir{tests/}: install = false
doc{INSTALL*}: install = false
