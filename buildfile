# file      : buildfile
# license   : MIT; see accompanying LICENSE file

./: {*/ -build/ -web/}                                     \
    doc{NEWS README INSTALL*} legal{LICENSE AUTHORS LEGAL} \
    manifest

# Don't install tests or the INSTALL* files.
#
tests/:        install = false
doc{INSTALL*}: install = false
