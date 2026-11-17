#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# libdiginet's desk emulator, its pump, its private-network helper and its child harness are
# libdiginet's (libdiginet-emu): no C file of this tree defines any of them again.
#
#   tests/diginet/no-emu-copy.sh [tree]
#   tests/diginet/no-emu-copy.sh --self-test
#
# Every tracked file of a git tree (every file of any other) is read. Exit 0 when none defines
# them, 1 naming each file that does.
#
# --self-test: this tree must pass; then, in a scratch copy, once with git metadata and once
# without (an unpacked release tarball), a planted copy of the emulator's header must turn it
# red naming that file, its removal must turn it green again, and a tree with nothing to read
# must be red. Exit 0 all of that held, 1 not.
set -u
if [ "${1:-}" = --self-test ]; then
	self=$(cd "$(dirname "$0")" && pwd -P)/$(basename "$0")
	top=$(cd "$(dirname "$0")/../.." && pwd -P)
	tmp=$(mktemp -d) || exit 2
	trap 'rm -rf "$tmp"' EXIT
	fail=0
	ck() { # name wanted-exit tree [grep-pattern]
		out=$(sh "$self" "$3" 2>&1); rc=$?
		if [ "$rc" -eq "$2" ] && { [ -z "${4:-}" ] || printf '%s\n' "$out" | grep -q "$4"; }; then
			echo "ok: $1 -> $rc"
		else
			echo "FAIL: $1 gave $rc, wanted $2: $out"; fail=1
		fi
	}
	ck "this tree" 0 "$top"
	if git -C "$top" rev-parse --show-toplevel 2>/dev/null | grep -qx "$top"; then
		files=$(cd "$top" && git ls-files -co --exclude-standard)
	else
		files=$(cd "$top" && find . -type f ! -path './.git/*' | sed 's|^\./||')
	fi
	for f in $files; do
		for k in git tarball; do
			mkdir -p "$tmp/$k/$(dirname "$f")" && cp -p "$top/$f" "$tmp/$k/$f" || exit 2
		done
	done
	git -C "$tmp/git" init -q
	[ ! -e "$tmp/tarball/.git" ] || { echo "NOT A RESULT: the tarball fixture carries .git"; exit 2; }
	for k in git tarball; do
		ck "$k copy as it is" 0 "$tmp/$k"
		printf '#ifndef DIGINET_DESK_EMU_H\n#define DIGINET_DESK_EMU_H\nstruct desk_emu {\n\tint fd;\n};\n#endif\n' \
			> "$tmp/$k/tests/diginet/desk_emu.h"
		ck "$k copy with desk_emu.h put back" 1 "$tmp/$k" '^FAIL tests/diginet/desk_emu.h '
		rm "$tmp/$k/tests/diginet/desk_emu.h"
		ck "$k copy with it removed" 0 "$tmp/$k"
	done
	# reading nothing is red: an empty tree, and a checkout git lists no file of
	mkdir -p "$tmp/empty" "$tmp/emptygit"; git -C "$tmp/emptygit" init -q
	ck "empty tree" 1 "$tmp/empty" 'nothing was read'
	ck "empty checkout" 1 "$tmp/emptygit" 'nothing was read'
	[ $fail -eq 0 ] && echo "no-emu-copy --self-test: a planted copy is red, its removal green"
	exit $fail
fi
top=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
cd "$top" || exit 2
if [ -d .git ] || git rev-parse --show-toplevel 2>/dev/null | grep -qx "$(pwd -P)"; then
	files=$(git ls-files -co --exclude-standard)
else
	files=$(find . -type f ! -path './.git/*' | sed 's|^\./||')
fi
[ -n "$files" ] || { echo "FAIL no file listed in $top, so nothing was read"; exit 1; }
# what only libdiginet-emu defines: the emulator's state and its first function, the pump, the
# harness's spawn, and the include guards of its four headers
pat='^[[:space:]]*(void[[:space:]]+emu_init[[:space:]]*\(|struct[[:space:]]+desk_emu[[:space:]]*\{|static[[:space:]]+inline[[:space:]]+void[[:space:]]+emu_pump_fd[[:space:]]*\(|static[[:space:]]+inline[[:space:]]+int[[:space:]]+emu_child_spawn[[:space:]]*\(|#[[:space:]]*define[[:space:]]+DIGINET_(DESK_EMU|EMU_PUMP|TEST_NETNS|EMU_CHILD)_H)'
bad=0
for f in $files; do
	case $f in *.c|*.h) ;; *) continue ;; esac
	if grep -qE "$pat" "$f"; then
		echo "FAIL $f defines libdiginet's desk emulator again: build against libdiginet-emu instead"
		bad=1
	fi
done
[ $bad -eq 0 ] && echo "no-emu-copy: no file defines libdiginet's desk emulator"
exit $bad
