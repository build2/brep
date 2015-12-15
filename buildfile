# file      : buildfile
# copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
# license   : MIT; see accompanying LICENSE file

d = brep/ loader/ tests/ www/
./: $d doc{LICENSE version}
include $d
