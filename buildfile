brep=brep/{diagnostics module options package package-odb search view}
web=web/apache/{request service}

libso{brep}: cxx{$brep $web services}
cxx.poptions += -I$src_root
cxx.libs += -lodb-pgsql -lodb
