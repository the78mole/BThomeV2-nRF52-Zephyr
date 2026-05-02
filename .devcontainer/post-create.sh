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
# 0. Berechtigungen des Named-Volume-Inhalts auf den aktuellen Benutzer setzen.
#    Das Named Volume wird von Docker initial als root:root angelegt; der
#    postCreateCommand läuft jedoch als non-root (vscode).  Wir chownen alle
#    Einträge im West-Workspace – ausgenommen den Bind-Mount des Projekts selbst,
#    da dieser auf dem Host-Dateisystem liegt.
# -----------------------------------------------------------------------------
CURRENT_UID="$(id -u)"
CURRENT_GID="$(id -g)"
PROJECT_BASENAME="$(basename "${PROJECT_DIR}")"

if [ "$(stat -c '%u' "${WEST_WORKSPACE}")" != "${CURRENT_UID}" ]; then
    echo ">>> [0/4] Berechtigungen des West-Workspace korrigieren (root → $(id -un)) ..."
    sudo chown "${CURRENT_UID}:${CURRENT_GID}" "${WEST_WORKSPACE}"
    find "${WEST_WORKSPACE}" -maxdepth 1 -mindepth 1 \
        ! -name "${PROJECT_BASENAME}" \
        -exec sudo chown -R "${CURRENT_UID}:${CURRENT_GID}" {} +
    echo "          Berechtigungen gesetzt."
else
    echo ">>> [0/4] Berechtigungen des West-Workspace bereits korrekt – übersprungen."
fi
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
#
#    Hintergrund: Die gemountete Host-.gitconfig enthält oft einen
#    plattformspezifischen credential.helper (manager-core, osxkeychain …),
#    der im Container nicht vorhanden ist.  Git fällt dann auf interaktive
#    Eingabe zurück und der Schritt hängt.
#    Lösung: credential.helper zurücksetzen und gh-CLI als Helper nutzen
#    (gh auth setup-git liest GITHUB_TOKEN, das im Devcontainer verfügbar ist).
# -----------------------------------------------------------------------------
echo ">>> [2/4] west update (NCS v2.6.0 + Zephyr + MCUboot) ..."
echo "    Hinweis: Erster Download kann 5–15 Minuten dauern."

# Plattformspezifischen Credential-Helper des Hosts deaktivieren.
# ~/.gitconfig ist als read-only Bind-Mount eingehängt → system-weite Konfiguration nutzen.
sudo git config --system credential.helper ""
# GitHub CLI als Credential-Helper einrichten (nutzt GITHUB_TOKEN).
gh auth setup-git 2>/dev/null || true

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
    -r "${WEST_WORKSPACE}/sdk-nrf/scripts/requirements.txt" \
    -r "${WEST_WORKSPACE}/bootloader/mcuboot/scripts/requirements.txt"

echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║   Setup abgeschlossen!  Viel Erfolg beim Entwickeln.         ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""
echo "  Tipp: Im nRF Connect Panel (VS Code) den Workspace unter"
echo "  '${WEST_WORKSPACE}' öffnen und ein Board auswählen."
echo ""
