#!/usr/bin/env bash
set -euo pipefail

BINARY=${1:?usage: deploy-win.sh BINARY INSTALLPATH WORKDIR}
INSTALLPATH=${2:?usage: deploy-win.sh BINARY INSTALLPATH WORKDIR}
WORKDIR=${3:?usage: deploy-win.sh BINARY INSTALLPATH WORKDIR}

if [[ -z ${DESTDIR:-} ]]; then
	OUTDIR=$INSTALLPATH
else
	if [[ $INSTALLPATH =~ ^[A-Za-z]:[/\\] ]]; then
		INSTALLPATH=${INSTALLPATH:3}
	fi
	OUTDIR="$WORKDIR/$DESTDIR/$INSTALLPATH"
fi
mkdir -p "$OUTDIR"

declare -a dlls=()
if command -v ntldd >/dev/null 2>&1; then
	mapfile -t dlls < <(ntldd -R "$BINARY" | grep -Ei 'mingw|ucrt' | cut -d">" -f2 | sed -e 's/(0x[0-9a-f]\+)//' -e 's/^ \+//' -e 's/ \+$//' -e 's,\\,/,g')
elif command -v gdb >/dev/null 2>&1; then
	mapfile -t dlls < <(gdb "$BINARY" --command="$(dirname "$0")/dlls.gdb" | grep -Ei 'mingw|ucrt' | cut -d" " -f7- | sed -e 's/^ \+//' -e 's/ \+$//' -e 's,\\,/,g')
else
	echo "Please install gdb or ntldd for deploying DLLs" >&2
	exit 1
fi

if ((${#dlls[@]})); then
	cp -vu "${dlls[@]}" "$OUTDIR"
fi

windeployqt=${WINDEPLOYQT:-}
if [[ -z $windeployqt ]]; then
	for candidate in windeployqt windeployqt-qt5 windeployqt6; do
		if command -v "$candidate" >/dev/null 2>&1; then
			windeployqt=$(command -v "$candidate")
			break
		fi
	done
fi

if [[ -z $windeployqt ]]; then
	echo "Please install windeployqt for deploying Qt plugins" >&2
	exit 1
fi
"$windeployqt" --no-angle --no-opengl-sw --no-svg --release --dir "$OUTDIR" "$BINARY"
