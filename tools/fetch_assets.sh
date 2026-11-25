#!/bin/bash
# SOLO uso personal: reemplaza las skins "Classic" (originales, ya en el repo) por las Standard de Delta
# de tu propio IPA, convertidas a PNG. Esas skins no tienen licencia para redistribuirse: no publiques
# un VPK compilado así. Para volver a las Classic: git checkout assets/skins
# Uso: tools/fetch_assets.sh /ruta/Delta.ipa
set -euo pipefail
IPA="${1:?Uso: $0 /ruta/Delta.ipa}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/assets/skins"; mkdir -p "$OUT"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
while read -r id path; do
  unzip -p "$IPA" "Payload/Delta.app/$path/Standard.deltaskin" > "$TMP/$id.deltaskin"
  python3 "$ROOT/tools/deltaskin2vita.py" "$TMP/$id.deltaskin" "$OUT/$id.deltaskin"
  echo "Skin $id"
done <<'LIST'
nes NESDeltaCore.bundle
snes SNESDeltaCore.bundle
gbc GBCDeltaCore.bundle
gba GBADeltaCore.bundle
n64 Mupen64Plus.bundle
ds DSDeltaCore.bundle
genesis Frameworks/Systems.framework/GPGXDeltaCore_GPGXDeltaCore.bundle
LIST
