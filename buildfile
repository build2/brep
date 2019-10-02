# file      : buildfile
# copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
# license   : MIT; see accompanying LICENSE file

./: {*/ -build/ -web/}                                \
    doc{LICENSE NEWS README INSTALL* CONTRIBUTING.md} \
    manifest

# Don't install tests or the INSTALL* files.
#
tests/:        install = false
doc{INSTALL*}: install = false
