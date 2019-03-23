#! /usr/bin/env bash

version=0.11.0-a.0.z
date="$(date +"%B %Y")"

trap 'exit 1' ERR
set -o errtrace # Trap in functions.

function info () { echo "$*" 1>&2; }
function error () { info "$*"; exit 1; }

while [ $# -gt 0 ]; do
  case $1 in
    --clean)
      rm -f brep*.xhtml brep*.1
      rm -f build2-repository-interface-manual*.ps \
         build2-repository-interface-manual*.pdf   \
         build2-repository-interface-manual.xhtml
      exit 0
      ;;
    *)
      error "unexpected $1"
      ;;
  esac
done

function compile ()
{
  local n=$1; shift

  # Use a bash array to handle empty arguments.
  #
  local o=()
  while [ $# -gt 0 ]; do
    o=("${o[@]}" "$1")
    shift
  done

  cli -I .. -v project="brep" -v version="$version" -v date="$date" \
--include-base-last "${o[@]}" --generate-html --html-prologue-file \
man-prologue.xhtml --html-epilogue-file man-epilogue.xhtml --html-suffix .xhtml \
--link-regex '%brep(#.+)?%build2-repository-interface-manual.xhtml$1%' \
../$n.cli

  cli -I .. -v project="brep" -v version="$version" -v date="$date" \
--include-base-last "${o[@]}" --generate-man --man-prologue-file \
man-prologue.1 --man-epilogue-file man-epilogue.1 --man-suffix .1 \
--link-regex '%brep(#.+)?%$1%' \
../$n.cli
}

o="--output-prefix brep-"

# A few special cases.
#
#compile "brep" $o --output-prefix ""

pages="clean/clean load/load migrate/migrate"

for p in $pages; do
  compile $p $o
done

# Manual.
#
cli -I .. \
-v version="$(echo "$version" | sed -e 's/^\([^.]*\.[^.]*\).*/\1/')" \
-v date="$date" \
--generate-html --html-suffix .xhtml \
--html-prologue-file doc-prologue.xhtml \
--html-epilogue-file doc-epilogue.xhtml \
--link-regex '%b([-.].+)%../../build2/doc/b$1%' \
--link-regex '%bpkg([-.].+)%../../bpkg/doc/bpkg$1%' \
--output-prefix build2-repository-interface- manual.cli

html2ps -f doc.html2ps:a4.html2ps -o build2-repository-interface-manual-a4.ps build2-repository-interface-manual.xhtml
ps2pdf14 -sPAPERSIZE=a4 -dOptimize=true -dEmbedAllFonts=true build2-repository-interface-manual-a4.ps build2-repository-interface-manual-a4.pdf

html2ps -f doc.html2ps:letter.html2ps -o build2-repository-interface-manual-letter.ps build2-repository-interface-manual.xhtml
ps2pdf14 -sPAPERSIZE=letter -dOptimize=true -dEmbedAllFonts=true build2-repository-interface-manual-letter.ps build2-repository-interface-manual-letter.pdf
