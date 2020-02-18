# file      : buildfile
# license   : MIT; see accompanying LICENSE file

./: {*/ -build/ -web/}                                        \
    doc{LICENSE AUTHORS NEWS README INSTALL* CONTRIBUTING.md} \
    manifest

# Don't install tests or the INSTALL* files.
#
tests/:        install = false
doc{INSTALL*}: install = false
