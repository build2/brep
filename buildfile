brep=brep/{diagnostics module options package package-odb search view}
web=web/apache/{request service}

import libs += libbpkg

libso{brep}: cxx{$brep $web services} $libs
cxx.poptions += -I$src_root
cxx.libs += -lodb-pgsql -lodb
