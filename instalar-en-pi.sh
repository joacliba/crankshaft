#!/usr/bin/env bash
# Compila e instala en la Raspberry Pi (Crankshaft v1.1.1-alpha.18) las mejoras:
#   - pinch-to-zoom (multitáctil) en Android Auto
#   - día/noche decidido por el móvil
#   - pantalla de inicio con reloj y ajustes más táctiles
#
# Uso, en la Pi, dentro de la carpeta descomprimida:
#   ./instalar-en-pi.sh            compila, prueba e instala
#   ./instalar-en-pi.sh --revertir vuelve a los paquetes originales guardados
#
# En una Pi 3B (1 GB) la primera compilación tarda bastante (en torno a 1 h o más).
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
ORIG="$DIR/paquetes-originales"
PKGS=(crankshaft-core crankshaft-ui-slim)

log() { printf '\n==> %s\n' "$*"; }

if [[ "${1:-}" == "--revertir" ]]; then
  log "Reinstalando los paquetes originales"
  sudo dpkg -i "$ORIG"/*.deb
  sudo systemctl restart crankshaft-core crankshaft-ui-slim
  exit 0
fi

# Guarda los .deb originales antes de tocar nada (para --revertir).
if [[ ! -d "$ORIG" ]]; then
  log "Guardando los paquetes originales en $ORIG"
  mkdir -p "$ORIG"
  (cd "$ORIG" && apt-get download "${PKGS[@]}") || {
    rmdir "$ORIG" 2>/dev/null || true
    echo "No he podido descargar los paquetes originales; sin ellos no habría vuelta atrás. Paro." >&2
    exit 1
  }
fi

# Con 1 GB de RAM el compilador se queda sin memoria: añade swap temporal si hace falta.
SWAP_TMP=/var/tmp/crankshaft-build.swap
swap_mb=$(awk '/SwapTotal/ {print int($2/1024)}' /proc/meminfo)
if (( swap_mb < 1500 )); then
  log "Añadiendo 2 GB de swap temporal ($SWAP_TMP)"
  sudo fallocate -l 2G "$SWAP_TMP" && sudo chmod 600 "$SWAP_TMP"
  sudo mkswap "$SWAP_TMP" >/dev/null && sudo swapon "$SWAP_TMP"
  trap 'sudo swapoff "$SWAP_TMP" 2>/dev/null; sudo rm -f "$SWAP_TMP"' EXIT
fi

# Para liberar memoria y CPU mientras compila.
sudo systemctl stop crankshaft-ui-slim crankshaft-core 2>/dev/null || true

JOBS="${JOBS:-2}"
for p in "${PKGS[@]}"; do
  log "Compilando $p (JOBS=$JOBS)"
  # --install-deps only installs and exits; the build is a second call.
  (cd "$DIR/$p" && ./build.sh --install-deps && BUILD_PACKAGE=ON BUILD_TESTS=ON JOBS="$JOBS" ./build.sh)
done

log "Instalando los paquetes nuevos"
sudo dpkg -i "$DIR"/crankshaft-core/build-release/packages/*.deb
sudo dpkg -i "$DIR"/crankshaft-ui-slim/build-release/packages/*.deb
sudo systemctl restart crankshaft-core crankshaft-ui-slim

log "Hecho. Si algo va mal: ./instalar-en-pi.sh --revertir"
