# Compiling the Godot backend

**WARNING**: the Godot rendering backend is **experimental** and lives on the
`godot-mig` branch. It is not an official build and it replaces the SDL renderer
rather than complementing it. For the supported builds see
[COMPILING.md](COMPILING.md).

Instead of an executable, this build produces a **GDExtension shared library**
that a Godot project loads: the game logic stays in C++ and Godot owns
rendering, windowing and input. See
[`docs/RENDERING_AND_GODOT_MIGRATION.md`](../../docs/RENDERING_AND_GODOT_MIGRATION.md)
for the architecture and
[`docs/godot_migration/architecture_adr.md`](../../docs/godot_migration/architecture_adr.md)
for the decisions behind it.

## Supported targets

| Target | How | Artifact |
|---|---|---|
| Linux x86_64 | native | `libcataclysm-godot.linux.debug.x86_64.so` |
| Windows x86_64 | MinGW-w64 cross-compile from Linux, or MSYS2 | `libcataclysm-godot.windows.debug.x86_64.dll` |
| macOS arm64 / x86_64 | native | `libcataclysm-godot.dylib` |

The names are not cosmetic: they are the ones
[`godot/extensions/cataclysm.gdextension`](../../godot/extensions/cataclysm.gdextension)
declares, and the build installs the library into `godot/bin/` under exactly
that name. A `RELEASE=1` build produces the `release` variant instead of `debug`.

`GODOT=1` cannot be combined with `TILES=1` or `SOUND=1`.

## Prerequisites

* A C++17 compiler (see [COMPILER_SUPPORT.md](COMPILER_SUPPORT.md)).
* **Godot 4.7.1-stable** — <https://godotengine.org/download/>. Any Godot 4.x at
  or above the `compatibility_minimum` in the `.gdextension` can load the
  extension, since GDExtension is compatible across 4.x.
* **CMake** and (optionally) **Ninja**, to build the bindings.
* **godot-cpp** at tag `godot-4.5-stable` — fetched for you by the script below.
* For the Windows cross-build from Linux, on Debian/Ubuntu:
  `sudo apt-get install mingw-w64 libz-mingw-w64-dev`.

The Godot build needs **no SDL and no curses library** at all.

## Step 1: build the godot-cpp bindings

```sh
./build-scripts/get-godot-cpp.sh
```

That clones godot-cpp into `~/godot-cpp` and builds it for the host platform.
Useful options (`--help` lists them all):

```sh
# A different location
./build-scripts/get-godot-cpp.sh --dir /path/to/godot-cpp

# Cross-compiled Windows bindings
./build-scripts/get-godot-cpp.sh --platform windows --arch x86_64

# Bindings for a RELEASE=1 game build
./build-scripts/get-godot-cpp.sh --target template_release
```

Native builds land in `<dir>/build`, cross-compiled ones in
`<dir>/build-<platform>-<arch>`; the game build looks in both.

> The bindings' `template_debug` / `template_release` choice must match the game
> build's `RELEASE` setting. godot-cpp exports `DEBUG_ENABLED` and
> `HOT_RELOAD_ENABLED` only from `template_debug`, and those macros change
> header-inline code — mixing them produces a library that links but misbehaves
> at runtime.

## Step 2: build the extension

The Makefile is the primary supported path.

```sh
# Linux (native)
make GODOT=1 NATIVE=linux64 -j$(nproc)

# macOS
make GODOT=1 -j$(sysctl -n hw.ncpu)

# Windows, cross-compiled from Linux
make GODOT=1 CROSS=x86_64-w64-mingw32- BACKTRACE=0 -j$(nproc)
```

`BACKTRACE=0` on the Windows cross-build avoids needing a MinGW libbacktrace;
drop it if you have one installed. The resulting DLL is self-contained — it links
the MinGW runtime, winpthread and zlib statically, so it needs no DLLs beside it.

Relevant variables:

| Variable | Default | Meaning |
|---|---|---|
| `GODOT_CPP_DIR` | `$HOME/godot-cpp` | godot-cpp checkout |
| `GODOT_CPP_BUILD_DIR` | auto | bindings build tree, if not in the usual place |
| `GODOT_ARCH` | auto | `x86_64` or `arm64`; override for a specific slice |
| `RELEASE` | unset | `1` selects the optimised `release` artifact |

The library is written to the repo root and copied to `godot/bin/`.

### CMake

The Makefile is the only supported path for the Godot backend. The CMake
`GODOT=ON` target exists and has had its hardcoded developer paths removed, but
it is **not maintained or verified** — use the Makefile.

## Step 3: run it

```sh
godot --path godot
```

On Apple Silicon, make sure the Godot binary's architecture matches the
extension's:

```sh
arch -arm64 /Applications/Godot.app/Contents/MacOS/Godot --path godot
```

### Checking that the library actually loads

**`godot --headless --path godot --quit` is not a valid check on its own.** On a
project with no `.godot/` cache it never verifies GDExtensions: it substitutes a
placeholder for `CDDAHost`, prints only downstream GDScript errors, and exits 0 —
so a library that cannot even be `dlopen`'d appears to pass.

Force the import pass instead, which runs Godot's "Verifying GDExtensions" step:

```sh
godot --headless --path godot --editor --quit
```

That is what reports the real problem, e.g. `Can't open dynamic library: … 
undefined symbol: endwin` or `Can't open GDExtension dynamic library`. Once it is
clean, the plain `--quit` run is meaningful and should reach `CDDA bootstrap
ready`.

A mismatch between the platform macros the bindings were built with and those the
extension was compiled with typically shows up here.

### Tileset data

The in-session map renders the **UltimateCataclysm** tileset from
`gfx/UltimateCataclysm/`. `/gfx/*` is gitignored, so a fresh checkout has to
compose it locally or unpack it from a
[CDDA-Tilesets](https://github.com/CleverRaven/Cataclysm-DDA-Tilesets) release.
Without it the map view stays empty; the rest of the UI still works.

## Notes

* Unit tests are not built for `GODOT=1`: the artifact is a shared library with
  no `main()`, so there is nothing for the test binary to link against. Run the
  test suite from a `TILES=1` or curses build.
* The GDExtension entry symbol is `godot_backend_extension_init`. If Godot
  reports a load failure, confirm it is exported:
  `nm -D --defined-only godot/bin/libcataclysm-godot.*.so | grep godot_backend`.
* `make clean` removes the extension artifacts and `godot/bin/`.
