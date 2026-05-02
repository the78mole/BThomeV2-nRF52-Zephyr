#!/usr/bin/env bash
# =============================================================================
# post-create.sh
# Initialisiert den NCS West-Workspace beim ersten Erstellen des Devcontainers.
#
# Aufruf (via devcontainer.json postCreateCommand):
#   bash post-create.sh <containerWorkspaceFolder>
#
# Ablauf:
#   1. west init  – lokaler Manifest-Modus (west.yml in diesem Repo)
#   2. west update – NCS v2.6.0 + Zephyr + MCUboot herunterladen (~3–5 GB)
#   3. west zephyr-export – CMake-Paket registrieren
#   4. uv pip install – Python-Abhängigkeiten in /opt/ncs/venv
# =============================================================================
set -euo pipefail

PROJECT_DIR="${1:?Fehler: containerWorkspaceFolder nicht angegeben.}"
WEST_WORKSPACE="$(dirname "${PROJECT_DIR}")"

echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║       nRF Connect SDK – Devcontainer Setup                   ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo "  Projektverzeichnis : ${PROJECT_DIR}"
echo "  West-Workspace     : ${WEST_WORKSPACE}"
echo "  VIRTUAL_ENV        : ${VIRTUAL_ENV:-/opt/ncs/venv}"
echo ""

# -----------------------------------------------------------------------------
# 1. West-Workspace initialisieren (lokaler Manifest-Modus)
#    Überspringen, falls bereits vorhanden (z. B. bei Container-Neustart
#    mit bestehendem Named Volume).
# -----------------------------------------------------------------------------
if [ ! -d "${WEST_WORKSPACE}/.west" ]; then
    echo ">>> [1/4] Initialisiere West-Workspace (west init -l) ..."
    west init -l "${PROJECT_DIR}"
else
    echo ">>> [1/4] West-Workspace bereits vorhanden – west init übersprungen."
fi

# -----------------------------------------------------------------------------
# 2. NCS-Quellcode herunterladen
#    Erstmalig ca. 3–5 GB; mit Named Volume wird dieser Schritt
#    bei Container-Neustarts deutlich schneller (nur Diff-Updates).
# -----------------------------------------------------------------------------
echo ">>> [2/4] west update (NCS v2.6.0 + Zephyr + MCUboot) ..."
echo "    Hinweis: Erster Download kann 5–15 Minuten dauern."
cd "${WEST_WORKSPACE}"
west update

# -----------------------------------------------------------------------------
# 3. Zephyr CMake-Paket exportieren
# -----------------------------------------------------------------------------
echo ">>> [3/4] west zephyr-export ..."
west zephyr-export

# -----------------------------------------------------------------------------
# 4. Python-Abhängigkeiten in die virtuelle Umgebung installieren
#    uv pip install respektiert VIRTUAL_ENV (/opt/ncs/venv).
#    west ist im Dockerfile bereits in die venv installiert; die West-Python-APIs
#    sind damit direkt importierbar und der CLI-Befehl über /opt/ncs/venv/bin verfügbar.
# -----------------------------------------------------------------------------
echo ">>> [4/4] Python-Abhängigkeiten via uv pip install ..."
# west ist bereits in der venv (Dockerfile); hier nur NCS-spezifische Requirements.
uv pip install \
    -r "${WEST_WORKSPACE}/zephyr/scripts/requirements.txt" \
    -r "${WEST_WORKSPACE}/nrf/scripts/requirements.txt" \
    -r "${WEST_WORKSPACE}/bootloader/mcuboot/scripts/requirements.txt"

echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║   Setup abgeschlossen!  Viel Erfolg beim Entwickeln.         ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""
echo "  Tipp: Im nRF Connect Panel (VS Code) den Workspace unter"
echo "  '${WEST_WORKSPACE}' öffnen und ein Board auswählen."
echo ""
