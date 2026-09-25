#!/usr/bin/env bash
#
# Compose gfx/<tileset>/ from tileset source art (SP-10).
#
# gfx/* is gitignored apart from the ASCII fallback, so a clean checkout has no
# graphical tileset and the Godot build silently falls back to ASCIITiles. Until
# now the fix was "download a release zip and unpack it by hand", which is not
# a build step, is not recorded anywhere, and produces an atlas nobody can
# reproduce or regenerate.
#
# This makes the atlas what it should be: a build artifact, derived from source
# art by a script, with the inputs named. It is also the prerequisite for doing
# anything with the art pipeline -- you cannot change how sprites are produced
# while the only copy of the output is a zip someone unpacked once.
#
# Usage:
#   build-scripts/compose-tileset.sh [tileset ...]     # default: UltimateCataclysm
#   build-scripts/compose-tileset.sh --list            # tilesets in the source tree
#
# Environment:
#   TILESET_SRC   existing CDDA-Tilesets checkout to use instead of cloning
#   TILESET_REPO  clone URL          (default: the upstream CDDA-Tilesets repo)
#   TILESET_REF   branch, tag or sha (default: master)
#   FORCE=1       recompose even when the output is already current
#
# Requires pyvips (the same dependency tools/gfx_tools/compose.py has always
# had). On Debian/Ubuntu:  sudo apt install libvips-dev && pip install pyvips
#
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

TILESET_REPO="${TILESET_REPO:-https://github.com/I-am-Erk/CDDA-Tilesets.git}"
TILESET_REF="${TILESET_REF:-master}"
default_src="$repo_root/cache/CDDA-Tilesets"
src="${TILESET_SRC:-$default_src}"
compose="$repo_root/tools/gfx_tools/compose.py"

die() { printf '%s: %s\n' "${BASH_SOURCE[0]##*/}" "$*" >&2; exit 1; }
note() { printf '\033[1m==>\033[0m %s\n' "$*"; }

check_deps() {
	command -v python3 >/dev/null || die "python3 not found"
	[ -f "$compose" ] || die "missing $compose"
	# compose.py imports pyvips at module scope, so a missing binding fails with
	# a bare ImportError several frames deep. Say it plainly instead.
	python3 - <<-'PY' || die "pyvips not importable -- install libvips and 'pip install pyvips'"
		import sys
		try:
		    import pyvips  # noqa: F401
		except ImportError:
		    try:
		        import gi
		        gi.require_version("Vips", "8.0")
		        from gi.repository import Vips  # noqa: F401
		    except Exception:
		        sys.exit(1)
	PY
}

# Fetch or update the source art.
#
# The upstream repo carries every tileset's source sprites -- tens of thousands
# of PNGs, gigabytes of history. A plain clone of it to build one tileset is
# not a reasonable build step, so this takes only what is asked for: no history
# (--depth 1), no blobs until checkout (--filter=blob:none), and only the
# gfx/<name> directories named on the command line (sparse-checkout).
ensure_source() {
	local want=("$@")
	if [ -n "${TILESET_SRC:-}" ]; then
		[ -d "$src" ] || die "TILESET_SRC=$src does not exist"
		note "using tileset source $src"
		return
	fi
	command -v git >/dev/null || die "git not found (or set TILESET_SRC to a checkout)"

	if [ ! -d "$src/.git" ]; then
		note "cloning $TILESET_REPO ($TILESET_REF) into $src"
		mkdir -p "$(dirname "$src")"
		git clone --depth 1 --branch "$TILESET_REF" --filter=blob:none \
			--sparse "$TILESET_REPO" "$src"
		git -C "$src" sparse-checkout init --cone
	else
		note "updating $src ($TILESET_REF)"
		git -C "$src" fetch --depth 1 origin "$TILESET_REF"
		git -C "$src" checkout -q FETCH_HEAD
	fi

	if [ ${#want[@]} -gt 0 ]; then
		local paths=()
		for name in "${want[@]}"; do
			paths+=("gfx/$name")
		done
		git -C "$src" sparse-checkout set "${paths[@]}"
	fi
}

source_version() {
	if [ -d "$src/.git" ]; then
		git -C "$src" rev-parse HEAD
	else
		# A plain directory has no commit to name, so fall back to the newest
		# mtime under it. Coarse, but it does change when the art does.
		find "$src/gfx/$1" -type f -newer "$src" -print -quit >/dev/null 2>&1 || true
		find "$src/gfx/$1" -type f -printf '%T@\n' 2>/dev/null | sort -rn | head -1
	fi
}

compose_one() {
	local name="$1"
	local from="$src/gfx/$name"
	local to="$repo_root/gfx/$name"
	local stamp="$to/.composed-from"

	[ -d "$from" ] || die "no tileset '$name' in $src/gfx (try --list)"

	local version
	version="$(source_version "$name")"
	if [ -z "${FORCE:-}" ] && [ -f "$to/tile_config.json" ] && [ -f "$stamp" ] &&
		[ "$(cat "$stamp")" = "$version" ]; then
		note "$name is already composed from $version -- skipping (FORCE=1 to redo)"
		return
	fi

	note "composing $name from $from"
	mkdir -p "$to"
	python3 "$compose" --use-all --feedback CONCISE "$from" "$to"

	# compose.py emits the tilesheets and tile_config.json and nothing else. The
	# rest of what makes a directory a tileset is hand-written and lives beside
	# the source sprites: tileset.txt is the manifest CDDA scans gfx/ for, so
	# without it the game does not see this tileset at all and silently falls
	# back to ASCIITiles -- which looks like the composition having failed.
	local copied=0
	for extra in tileset.txt fallback.png layering.json; do
		if [ -f "$from/$extra" ]; then
			cp "$from/$extra" "$to/$extra"
			copied=$((copied + 1))
		fi
	done
	[ -f "$to/tileset.txt" ] || die "$from has no tileset.txt; CDDA will not see $name"

	printf '%s\n' "$version" >"$stamp"
	note "$name -> gfx/$name ($(find "$to" -name '*.png' | wc -l | tr -d ' ') sheets)"
}

if [ "${1:-}" = "--list" ]; then
	ensure_source
	# ls would only show what sparse-checkout has materialised, which is the
	# opposite of what --list is for.
	if [ -d "$src/.git" ]; then
		git -C "$src" ls-tree --name-only HEAD gfx/ | sed 's|^gfx/||'
	else
		ls -1 "$src/gfx"
	fi
	exit 0
fi

tilesets=("$@")
if [ ${#tilesets[@]} -eq 0 ]; then
	# The one the Godot backend asks for by name; see MapSnapshot::ensure_tileset_loaded.
	tilesets=(UltimateCataclysm)
fi

check_deps
ensure_source "${tilesets[@]}"
for name in "${tilesets[@]}"; do
	compose_one "$name"
done
