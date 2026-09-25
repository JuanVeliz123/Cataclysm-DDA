#!/usr/bin/env bash
# Compile every GDScript and every shader in godot/, and fail if any is broken.
#
# This exists because the gap it closes cost three round trips. `gdparse`
# (gdtoolkit) checks *syntax* and nothing else, so a script full of misspelled
# members, wrong enum names or Transform2D methods called on a Transform3D parses
# perfectly and then fails to compile at load -- at which point `set_script()`
# leaves the node with no working script, `has_method("setup")` answers false, and
# the caller carries on silently. What that looks like from the outside is a blank
# map and a probe stage that "never ran": no error the reader connects to a script,
# and nothing to grep for.
#
# Godot's own front end answers it in seconds and needs neither the GDExtension nor
# a GPU. Run this before handing anything over.
#
#   ./build-scripts/check-godot-scripts.sh              # find Godot on PATH
#   GODOT=/path/to/Godot ./build-scripts/check-godot-scripts.sh
#
# Exit status is 0 when everything compiled and 1 otherwise.

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
project="$here/godot"

# Candidates are accepted only if they answer --version with something that looks
# like one. Testing for "is on PATH" or "is executable" is how the first version of
# this script reported "32 scripts compile" while running a command that did not
# exist: no output means no error lines, and no error lines read as success. A check
# that cannot tell "passed" from "never ran" is the thing this file exists to stop.
godot=""
for candidate in "${GODOT:-}" godot godot4 \
    "$HOME/Downloads/Godot.app/Contents/MacOS/Godot" \
    "/Applications/Godot.app/Contents/MacOS/Godot" \
    "$HOME/Applications/Godot.app/Contents/MacOS/Godot"; do
    [ -z "$candidate" ] && continue
    version="$("$candidate" --version 2>/dev/null | tail -1)"
    if [[ "$version" =~ [0-9]+\.[0-9]+ ]]; then
        godot="$candidate"
        break
    fi
done
if [ -z "$godot" ]; then
    echo "No working Godot binary found. Set GODOT=/path/to/Godot." >&2
    exit 1
fi
echo "godot: $godot ($version)"

# The GDExtension is not needed to compile scripts, and on a machine that has not
# built it the loader complains once per run. Drop those lines; keep everything else.
noise='GDExtension|libcataclysm|open_dynamic_library|cataclysm.gdextension|core/extension'

status=0

for path in "$project"/scripts/*.gd; do
    name="$(basename "$path")"
    out="$("$godot" --headless --path "$project" --check-only \
        --script "res://scripts/$name" 2>&1 |
        grep -Ev "$noise" | grep -E 'SCRIPT ERROR|Parse Error|Failed to load script')"
    if [ -n "$out" ]; then
        echo "FAIL $name"
        echo "$out" | sed 's/^/     /'
        status=1
    fi
done
[ $status -eq 0 ] && echo "ok: $(ls "$project"/scripts/*.gd | wc -l | tr -d ' ') scripts compile"

# Shaders have their own scene, which walks res://shaders/ and reports each one. It
# needs no extension either, so it belongs in the same gate. Judged on the summary
# line it prints rather than on its exit status, so a scene that dies early cannot
# pass by staying quiet.
shader_out="$("$godot" --headless --path "$project" res://scenes/shader_check.tscn 2>&1 |
    grep -Ev "$noise")"
echo "$shader_out" | grep -E '^\[shader\]|SHADER ERROR' || true
if ! echo "$shader_out" | grep -qE 'shader\(s\) compiled'; then
    echo "FAIL shader_check did not report a compile summary" >&2
    status=1
fi

# The 3D backend's geometry (ADR-006 option B): place a sprite, project it back
# through the camera, and check the rectangle comes home. Needs no extension and no
# GPU either -- unproject_position is maths on a transform -- which is what lets the
# tilt be verified before the light that makes it visible exists.
geom_out="$("$godot" --headless --path "$project" res://scenes/geometry_check.tscn 2>&1 |
    grep -Ev "$noise")"
echo "$geom_out" | grep -E 'round-tripped|^\[geom\] FAIL' || true
if ! echo "$geom_out" | grep -qE 'round-tripped'; then
    echo "FAIL geometry_check did not report a round-trip summary" >&2
    status=1
fi

# The C++ side, syntax-only. Needs godot-cpp's headers but neither its static library
# nor a link step, so it runs in seconds -- which matters, because until this existed the
# only gate on the backend's C++ was somebody else's rebuild. Skipped with a note rather
# than a failure when the headers are absent, since not every checkout has them.
cpp_headers="${GODOT_CPP_DIR:-$HOME/godot-cpp}"
if [ -d "$cpp_headers/build/gen/include" ]; then
    cpp_status=0
    for f in "$here"/src/godot_*.cpp "$here"/godot/extensions/*.cpp; do
        out="$(clang++ -fsyntax-only -std=c++17 -DGODOT -DGDEXTENSION -DLOCALIZE \
            -I"$here/src" -I"$here/src/third-party" \
            -isystem "$cpp_headers/include" \
            -isystem "$cpp_headers/build/gen/include" \
            -isystem "$cpp_headers/gdextension" "$f" 2>&1 | grep -E 'error:' | head -5)"
        if [ -n "$out" ]; then
            echo "FAIL $(basename "$f")"
            echo "$out" | sed 's/^/     /'
            cpp_status=1
        fi
    done
    [ $cpp_status -eq 0 ] && echo "ok: $(ls "$here"/src/godot_*.cpp "$here"/godot/extensions/*.cpp | wc -l | tr -d ' ') C++ files compile (syntax only)"
    [ $cpp_status -ne 0 ] && status=1
else
    echo "skip: no godot-cpp headers at $cpp_headers -- C++ not checked"
    echo "      ./build-scripts/get-godot-cpp.sh, or set GODOT_CPP_DIR"
fi

exit $status
