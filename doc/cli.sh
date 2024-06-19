#! /usr/bin/env bash

version=0.18.0-a.0.z

trap 'exit 1' ERR
set -o errtrace # Trap in functions.

function info () { echo "$*" 1>&2; }
function error () { info "$*"; exit 1; }

date="$(date +"%B %Y")"
copyright="$(sed -n -re 's%^Copyright \(c\) (.+) \(see the AUTHORS and LEGAL files\)\.$%\1%p' ../LICENSE)"

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

  cli -I .. \
-v project="brep" \
-v version="$version" \
-v date="$date" \
-v copyright="$copyright" \
--include-base-last "${o[@]}" \
--generate-html --html-suffix .xhtml \
--html-prologue-file man-prologue.xhtml \
--html-epilogue-file man-epilogue.xhtml \
--link-regex '%bpkg([-.].+)%../../bpkg/doc/bpkg$1%' \
--link-regex '%brep(#.+)?%build2-repository-interface-manual.xhtml$1%' \
../$n.cli

  cli -I .. \
-v project="brep" \
-v version="$version" \
-v date="$date" \
-v copyright="$copyright" \
--include-base-last "${o[@]}" \
--generate-man --man-suffix .1 \
--man-prologue-file man-prologue.1 \
--man-epilogue-file man-epilogue.1 \
--link-regex '%bpkg(#.+)?%$1%' \
--link-regex '%brep(#.+)?%$1%' \
--link-regex '%bbot(#.+)?%$1%' \
../$n.cli
}

o="--output-prefix brep-"

# A few special cases.
#
#compile "brep" $o --output-prefix ""

pages="clean/clean load/load migrate/migrate monitor/monitor"

for p in $pages; do
  compile $p $o
done

# Manual.
#
function xhtml_to_ps () # <from> <to> [<html2ps-options>]
{
  local from="$1"
  shift
  local to="$1"
  shift

  sed -e 's/├/|/g' -e 's/│/|/g' -e 's/─/-/g' -e 's/└/\xb7/g' "$from" | \
  html2ps "${@}" -o "$to"
}

cli -I .. \
-v version="$(echo "$version" | sed -e 's/^\([^.]*\.[^.]*\).*/\1/')" \
-v date="$date" \
-v copyright="$copyright" \
--generate-html --html-suffix .xhtml \
--html-prologue-file doc-prologue.xhtml \
--html-epilogue-file doc-epilogue.xhtml \
--link-regex '%b([-.].+)%../../build2/doc/b$1%' \
--link-regex '%bpkg([-.].+)%../../bpkg/doc/bpkg$1%' \
--link-regex '%bpkg(#.+)?%../../bpkg/doc/build2-package-manager-manual.xhtml$1%' \
--link-regex '%bbot(#.+)?%../../bbot/doc/build2-build-bot-manual.xhtml$1%' \
--output-prefix build2-repository-interface- \
manual.cli

xhtml_to_ps build2-repository-interface-manual.xhtml build2-repository-interface-manual-a4.ps -f doc.html2ps:a4.html2ps
ps2pdf14 -sPAPERSIZE=a4 -dOptimize=true -dEmbedAllFonts=true build2-repository-interface-manual-a4.ps build2-repository-interface-manual-a4.pdf

xhtml_to_ps build2-repository-interface-manual.xhtml build2-repository-interface-manual-letter.ps -f doc.html2ps:letter.html2ps
ps2pdf14 -sPAPERSIZE=letter -dOptimize=true -dEmbedAllFonts=true build2-repository-interface-manual-letter.ps build2-repository-interface-manual-letter.pdf
