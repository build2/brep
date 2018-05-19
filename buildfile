# file      : buildfile
# copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
# license   : MIT; see accompanying LICENSE file

./: {*/ -build/ -web/} doc{LICENSE NEWS README INSTALL*} manifest

# Don't install tests or the INSTALL* files.
#
dir{tests/}:   install = false
doc{INSTALL*}: install = false
