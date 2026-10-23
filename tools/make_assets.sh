#!/bin/sh
# Gera assets/ (fontes, logo e botoes). Rodar no conteiner:
#   docker compose run --rm ps2dvdplayer sh tools/make_assets.sh
set -e

apk add -q --no-cache font-noto py3-pillow >/dev/null 2>&1

# Fontes (OFL) copiadas para o repositorio: os atlas e o logo derivam delas.
mkdir -p third_party/fonts
cp /usr/share/fonts/noto/NotoSans-Regular.ttf third_party/fonts/
cp /usr/share/fonts/noto/NotoSans-Bold.ttf third_party/fonts/

python3 tools/make_assets.py
