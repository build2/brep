# file      : buildfile
# license   : MIT; see accompanying LICENSE file

./: {*/ -build/ -web/}                              \
    doc{LICENSE AUTHORS LEGAL NEWS README INSTALL*} \
    manifest

# Don't install tests or the INSTALL* files.
#
tests/:        install = false
doc{INSTALL*}: install = false
