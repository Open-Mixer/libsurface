#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# Floors only (owner ruling 2026-11-16): the packaging of this repository states a floor on every package of ours it
# needs and no ceiling. Two arms:
#   ARM 1  no `<`, `<<` or `<=` bound on libdiginet*, libmackie*, diginet-*, omx-*, plugin-*, libreac*, reac-* or
#          mod-host* in packaging/ (spec, *.pc.in) or in debian/control (changelogs and comments are history);
#   ARM 2  the Makefile and BUILDING.md name no `freemixer-dotgithub` default for the shared checker: the default is
#          the cache clone $HOME/.cache/omx/openmixer-dotgithub.
# It reads files only. A planted ceiling and a planted stale default run first and must be refused, so the check
# cannot pass vacuously.
#
# Exit 0 conforms, 1 an arm found an offence, 2 the planted copy was not refused (NOT A RESULT).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OURS='(lib)?(diginet|mackie|omx|plugin|reac|mod-host)[A-Za-z0-9._+-]*'

ceilings() {   # <file...>: the offending lines
	grep -nE "(^|[^A-Za-z0-9_-])($OURS)(%\{\?_isa\})?\)? *\(?(<<|<=|<) *[0-9]" "$@" 2>/dev/null |
		grep -vE '^[^:]*:[0-9]+: *#' || true
}

check() {
	t=$1 bad=0
	out=$(ceilings "$t"/packaging/*.spec "$t"/packaging/*.pc.in "$t/debian/control" |
		grep -vE '^[^:]*spec:[0-9]+:(- |  )' || true)
	if [ -n "$out" ]; then
		echo "  ARM 1: a ceiling on a package of ours:"
		echo "$out" | sed 's/^/    /'
		bad=1
	fi
	out=$(grep -n 'freemixer-dotgithub' "$t/Makefile" "$t/BUILDING.md" 2>/dev/null || true)
	if [ -n "$out" ]; then
		echo "  ARM 2: a freemixer-dotgithub default:"
		echo "$out" | sed 's/^/    /'
		bad=1
	fi
	return $bad
}

tmp=$(mktemp -d) || exit 2
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/packaging" "$tmp/debian"
printf 'Requires: (libmackie >= 0.1.0 with libmackie < 0.2)\n' > "$tmp/packaging/x.spec"
printf 'Package: p\nDepends: libdiginet-dev (>= 1~), libdiginet-dev (<< 2~), libc6 (<< 9)\n' > "$tmp/debian/control"
printf 'OPENMIXER_DOTGITHUB ?= ../freemixer-dotgithub\n' > "$tmp/Makefile"
if check "$tmp" >/dev/null; then
	echo "NOT A RESULT: the planted ceiling and stale default were not refused"
	exit 2
fi
rm -rf "$tmp/packaging" "$tmp/debian" "$tmp/Makefile"
mkdir -p "$tmp/packaging" "$tmp/debian"
printf 'Requires: (libmackie >= 0.1.0)\nRequires: (glibc >= 2 with glibc < 3)\n' > "$tmp/packaging/x.spec"
printf 'Package: p\nDepends: libdiginet-dev (>= 1~), libc6 (<< 9)\n' > "$tmp/debian/control"
if ! check "$tmp" >/dev/null; then
	echo "NOT A RESULT: a floor, or a bound on a third-party package, was refused"
	exit 2
fi

if check "$ROOT"; then
	echo "ok floors only, and no freemixer-dotgithub default"
	exit 0
fi
exit 1
