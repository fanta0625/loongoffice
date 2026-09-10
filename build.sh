#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$root"

# Prepare the pinned sources and external tarballs first, as documented in
# debian/README.source. debian/rules validates those inputs before compiling.
exec dpkg-buildpackage -b -us -uc "$@"
