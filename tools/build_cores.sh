#!/bin/bash
# Compila los núcleos libretro como librerías estáticas para Vita (platform=vita) en cores/lib/.
# Uso: tools/build_cores.sh [nombre...]   (sin argumentos: todos)
set -uo pipefail
export VITASDK="${VITASDK:-$HOME/vitasdk}"; export PATH="$VITASDK/bin:$PATH"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; SRC="$ROOT/cores/src"; LIB="$ROOT/cores/lib"
mkdir -p "$SRC" "$LIB"
# el frontend compila libretro-common (los núcleos estáticos no lo traen)
[ -d "$SRC/libretro-common" ] || git clone -q --depth 1 https://github.com/libretro/libretro-common "$SRC/libretro-common"
# nombre | repo libretro | carpeta del makefile | makefile | .a resultante
CORES='
fceumm|libretro-fceumm|.|Makefile.libretro|fceumm_libretro_vita.a
snes9x2005|snes9x2005|.|Makefile|snes9x2005_libretro_vita.a
gambatte|gambatte-libretro|.|Makefile.libretro|gambatte_libretro_vita.a
gpsp|gpsp|.|Makefile|gpsp_libretro_vita.a
genesis_plus_gx|Genesis-Plus-GX|.|Makefile.libretro|genesis_plus_gx_libretro_vita.a
desmume2015|desmume2015|desmume|Makefile.libretro|desmume2015_libretro_vita.a
parallel_n64|parallel-n64|.|Makefile|parallel_n64_libretro_vita.a
'
want=" ${*:-} "
echo "$CORES" | while IFS='|' read -r name repo dir mk out; do
  [ -z "$name" ] && continue
  [ -n "${*:-}" ] && [[ "$want" != *" $name "* ]] && continue
  [ -s "$LIB/$name.a" ] && { echo "[ok] $name (ya compilado)"; continue; }
  [ -d "$SRC/$name" ] || git clone -q --depth 1 "https://github.com/libretro/$repo" "$SRC/$name" || { echo "[FAIL] clone $name"; continue; }
  # este vitasdk y sus librerías son softfp; algunos Makefiles fuerzan hard-float en su sección vita
  perl -0pi -e 's/(platform\), vita\).*?)(?=\n(?:else )?ifeq|\n# )/ my $b=$1; $b=~s#-mfloat-abi=hard#-mfloat-abi=softfp#g; $b /se' "$SRC/$name/$dir/$mk"
  echo "[..] $name"
  if (cd "$SRC/$name/$dir" && make -f "$mk" platform=vita -j"$(sysctl -n hw.ncpu)" > "$LIB/$name.log" 2>&1); then
    f=$(find "$SRC/$name" -name "$out" | head -1)
    [ -n "$f" ] && cp "$f" "$LIB/$name.a" && echo "[ok] $name" || echo "[FAIL] $name: no se encontró $out"
  else
    echo "[FAIL] $name (ver cores/lib/$name.log)"
  fi
done
