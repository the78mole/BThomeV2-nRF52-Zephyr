#!/usr/bin/env bash
# =============================================================================
# check-prerequisites.sh
# Prüft alle Voraussetzungen für den nRF Connect SDK Devcontainer.
#
# Aufruf:
#   bash .devcontainer/check-prerequisites.sh [--quiet]
#
# Exit-Code:
#   0 – alle Pflicht-Checks bestanden
#   1 – mindestens ein Pflicht-Check fehlgeschlagen
#
# Kategorien:
#   [REQUIRED] – ohne diese ist kein Build möglich
#   [OPTIONAL] – fehlt, aber der Build kann trotzdem laufen
# =============================================================================
set -uo pipefail

# ---------------------------------------------------------------------------
# Farben & Symbole
# ---------------------------------------------------------------------------
if [ -t 1 ]; then
    RED='\033[0;31m'; YELLOW='\033[1;33m'; GREEN='\033[0;32m'
    CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'
else
    RED=''; YELLOW=''; GREEN=''; CYAN=''; BOLD=''; RESET=''
fi

QUIET=0
[[ "${1:-}" == "--quiet" ]] && QUIET=1

PASS=0; FAIL=0; WARN=0

_ok()   { PASS=$((PASS+1)); [[ $QUIET -eq 0 ]] && printf "  ${GREEN}✔${RESET}  %s\n" "$*"; }
_fail() { FAIL=$((FAIL+1)); printf "  ${RED}✘${RESET}  %s\n" "$*"; }
_warn() { WARN=$((WARN+1)); [[ $QUIET -eq 0 ]] && printf "  ${YELLOW}⚠${RESET}  %s\n" "$*"; }
_head() { [[ $QUIET -eq 0 ]] && printf "\n${BOLD}${CYAN}%s${RESET}\n" "$*"; }

# Versionsnummer aus String extrahieren (erste x.y.z-Gruppe)
_ver() { echo "$*" | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1; }

# Versionsvergleich: 1 wenn $1 >= $2 (major.minor.patch)
_ver_gte() {
    local IFS=.
    local -a a=($1) b=($2)
    for i in 0 1 2; do
        local av="${a[$i]:-0}" bv="${b[$i]:-0}"
        (( av > bv )) && return 0
        (( av < bv )) && return 1
    done
    return 0
}

# ---------------------------------------------------------------------------
# Banner
# ---------------------------------------------------------------------------
[[ $QUIET -eq 0 ]] && cat <<'EOF'

╔══════════════════════════════════════════════════════════════════════════╗
║     nRF Connect SDK Devcontainer – Prerequisites Check                   ║
║     NCS v2.6.0 / Zephyr 3.5.99-ncs1 / Zephyr SDK 0.16.5                ║
╚══════════════════════════════════════════════════════════════════════════╝
EOF

# ===========================================================================
# 1. Build-Tools (cmake, ninja, gperf, dtc, make, gcc)
# ===========================================================================
_head "1. Build-Tools [REQUIRED]"

# cmake ≥ 3.21
if cmd_out=$(cmake --version 2>/dev/null); then
    ver=$(_ver "$cmd_out")
    if _ver_gte "$ver" "3.21.0"; then
        _ok "cmake $ver (≥ 3.21 ✓)"
    else
        _fail "[REQUIRED] cmake $ver – mindestens 3.21 erforderlich"
    fi
else
    _fail "[REQUIRED] cmake nicht gefunden"
fi

# ninja ≥ 1.10.2
if cmd_out=$(ninja --version 2>/dev/null); then
    ver=$(_ver "$cmd_out")
    if _ver_gte "$ver" "1.10.2"; then
        _ok "ninja $ver (≥ 1.10.2 ✓)"
    else
        _fail "[REQUIRED] ninja $ver – mindestens 1.10.2 erforderlich"
    fi
else
    _fail "[REQUIRED] ninja nicht gefunden"
fi

# gperf ≥ 3.1
if cmd_out=$(gperf --version 2>/dev/null); then
    ver=$(_ver "$cmd_out")
    if _ver_gte "$ver" "3.1"; then
        _ok "gperf $ver (≥ 3.1 ✓)"
    else
        _fail "[REQUIRED] gperf $ver – mindestens 3.1 erforderlich"
    fi
else
    _fail "[REQUIRED] gperf nicht gefunden"
fi

# dtc ≥ 1.4.7
if cmd_out=$(dtc --version 2>/dev/null); then
    ver=$(_ver "$cmd_out")
    if _ver_gte "$ver" "1.4.7"; then
        _ok "dtc $ver (≥ 1.4.7 ✓)"
    else
        _fail "[REQUIRED] dtc $ver – mindestens 1.4.7 erforderlich"
    fi
else
    _fail "[REQUIRED] dtc (device-tree-compiler) nicht gefunden"
fi

# make
if make --version &>/dev/null; then
    _ok "make $(make --version | head -1 | grep -oE '[0-9]+\.[0-9]+')"
else
    _fail "[REQUIRED] make nicht gefunden"
fi

# gcc (Host-Compiler, für Zephyr-Host-Tools)
if cmd_out=$(gcc --version 2>/dev/null); then
    ver=$(_ver "$cmd_out")
    _ok "gcc $ver"
else
    _fail "[REQUIRED] gcc nicht gefunden"
fi

# ===========================================================================
# 2. Zephyr SDK
# ===========================================================================
_head "2. Zephyr SDK [REQUIRED]"

SDK_DIR="${ZEPHYR_SDK_INSTALL_DIR:-/opt/zephyr-sdk-0.16.5}"
if [ -d "$SDK_DIR" ]; then
    _ok "Zephyr SDK-Verzeichnis: $SDK_DIR"
else
    _fail "[REQUIRED] Zephyr SDK nicht gefunden: $SDK_DIR"
fi

# ARM-Toolchain
ARM_GCC="$SDK_DIR/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc"
if [ -x "$ARM_GCC" ]; then
    ver=$(_ver "$("$ARM_GCC" --version 2>/dev/null)")
    _ok "arm-zephyr-eabi-gcc $ver"
else
    _fail "[REQUIRED] arm-zephyr-eabi-gcc nicht gefunden: $ARM_GCC"
fi

# CMake-Paket registriert?
if cmake --find-package -DNAME=Zephyr-sdk -DCOMPILER_ID=GNU -DLANGUAGE=C \
        -DMODE=EXIST &>/dev/null 2>&1 \
   || [ -f "$SDK_DIR/zephyr-sdk-x86_64-hosttools-standalone-0.9.5.run" ] \
   || find /usr/share/cmake /usr/local/share/cmake "$HOME/.cmake" \
        -name "ZephyrConfig.cmake" 2>/dev/null | grep -q .; then
    _ok "Zephyr CMake-Paket registriert (cmake --find-package)"
else
    _warn "[OPTIONAL] Zephyr CMake-Paket evtl. nicht registriert – 'west zephyr-export' ausführen"
fi

# ZEPHYR_TOOLCHAIN_VARIANT
if [ "${ZEPHYR_TOOLCHAIN_VARIANT:-}" = "zephyr" ]; then
    _ok "ZEPHYR_TOOLCHAIN_VARIANT=zephyr"
else
    _warn "[OPTIONAL] ZEPHYR_TOOLCHAIN_VARIANT='${ZEPHYR_TOOLCHAIN_VARIANT:-<nicht gesetzt>}' (erwartet: zephyr)"
fi

# ===========================================================================
# 3. Python / uv / virtuelle Umgebung
# ===========================================================================
_head "3. Python / uv / venv [REQUIRED]"

# uv
if cmd_out=$(uv --version 2>/dev/null); then
    ver=$(_ver "$cmd_out")
    _ok "uv $ver"
else
    _fail "[REQUIRED] uv nicht gefunden"
fi

# VIRTUAL_ENV gesetzt und vorhanden
VENV="${VIRTUAL_ENV:-/opt/ncs/venv}"
if [ -d "$VENV" ]; then
    _ok "venv vorhanden: $VENV"
else
    _fail "[REQUIRED] venv nicht gefunden: $VENV"
fi

# Python-Interpreter in venv
VENV_PY="$VENV/bin/python3"
if [ -x "$VENV_PY" ]; then
    ver=$(_ver "$("$VENV_PY" --version 2>&1)")
    if _ver_gte "$ver" "3.10"; then
        _ok "venv python $ver (≥ 3.10 ✓)"
    else
        _fail "[REQUIRED] venv python $ver – mindestens 3.10 erforderlich"
    fi
else
    _fail "[REQUIRED] $VENV_PY nicht gefunden"
fi

# west in venv
if "$VENV/bin/west" --version &>/dev/null; then
    ver=$(_ver "$("$VENV/bin/west" --version 2>&1)")
    _ok "west $ver"
else
    _fail "[REQUIRED] west nicht in venv ($VENV/bin/west)"
fi

# PyYAML (wird von west und Zephyr-Skripten benötigt)
if "$VENV_PY" -c "import yaml" 2>/dev/null; then
    _ok "PyYAML importierbar"
else
    _fail "[REQUIRED] PyYAML fehlt in venv – 'uv pip install pyyaml' ausführen"
fi

# numpy ≥ 2.0 (für ppk_analysis.py)
if cmd_out=$("$VENV_PY" -c "import numpy; print(numpy.__version__)" 2>/dev/null); then
    if _ver_gte "$cmd_out" "2.0"; then
        _ok "numpy $cmd_out (≥ 2.0 ✓)"
    else
        _warn "[OPTIONAL] numpy $cmd_out – ppk_analysis.py benötigt ≥ 2.0"
    fi
else
    _warn "[OPTIONAL] numpy nicht in venv – ppk_analysis.py nicht nutzbar"
fi

# ===========================================================================
# 4. West-Workspace
# ===========================================================================
_head "4. West-Workspace [REQUIRED]"

WEST_WS="$(dirname "${WORKSPACE_DIR:-/workspaces/BThomeV2-nRF52-Zephyr}")"
WEST_WS="${WEST_WS:-/workspaces}"

if [ -d "$WEST_WS/.west" ]; then
    _ok "West-Workspace initialisiert: $WEST_WS/.west"
else
    _fail "[REQUIRED] .west nicht gefunden in $WEST_WS – 'west init -l .' ausführen"
fi

for repo in zephyr nrf bootloader/mcuboot modules/lib/zcbor; do
    if [ -d "$WEST_WS/$repo" ]; then
        _ok "$repo vorhanden"
    else
        _fail "[REQUIRED] $WEST_WS/$repo fehlt – 'west update' ausführen"
    fi
done

# ZEPHYR_BASE
ZEPHYR_BASE_EFF="${ZEPHYR_BASE:-$WEST_WS/zephyr}"
if [ -f "$ZEPHYR_BASE_EFF/CMakeLists.txt" ]; then
    _ok "ZEPHYR_BASE=$ZEPHYR_BASE_EFF"
else
    _warn "[OPTIONAL] ZEPHYR_BASE nicht korrekt gesetzt oder zephyr unvollständig"
fi

# ===========================================================================
# 5. nRF-Tools (nrfjprog / nRF Command Line Tools)
# ===========================================================================
_head "5. nRF Command Line Tools [OPTIONAL]"

if cmd_out=$(nrfjprog --version 2>/dev/null); then
    ver=$(echo "$cmd_out" | grep -oE 'nrfjprog version: [0-9]+\.[0-9]+\.[0-9]+' | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
    _ok "nrfjprog ${ver:-$(echo "$cmd_out" | _ver)}"
else
    _warn "[OPTIONAL] nrfjprog nicht gefunden – Flashen über J-Link nicht möglich"
fi

if mergehex --version &>/dev/null; then
    _ok "mergehex verfügbar"
else
    _warn "[OPTIONAL] mergehex nicht gefunden"
fi

# J-Link Runtime
if [ -f /usr/lib/libjlinkarm.so ] || ldconfig -p 2>/dev/null | grep -q libjlinkarm; then
    _ok "J-Link Bibliothek (libjlinkarm) gefunden"
else
    _warn "[OPTIONAL] J-Link Bibliothek nicht gefunden – nrfjprog kann trotzdem über nrfjprog-Bundle laufen"
fi

# ===========================================================================
# 6. GitHub CLI
# ===========================================================================
_head "6. GitHub CLI [OPTIONAL]"

if cmd_out=$(gh --version 2>/dev/null); then
    ver=$(_ver "$cmd_out")
    _ok "gh $ver"
    if gh auth status &>/dev/null; then
        user=$(gh api user -q .login 2>/dev/null || echo "unbekannt")
        _ok "gh authentifiziert als: $user"
    else
        _warn "[OPTIONAL] gh nicht authentifiziert – 'gh auth login' ausführen"
    fi
else
    _warn "[OPTIONAL] gh (GitHub CLI) nicht gefunden"
fi

# ===========================================================================
# 7. Git-Konfiguration
# ===========================================================================
_head "7. Git-Konfiguration [REQUIRED]"

if git --version &>/dev/null; then
    ver=$(_ver "$(git --version)")
    _ok "git $ver"
else
    _fail "[REQUIRED] git nicht gefunden"
fi

# safe.directory
if git config --system safe.directory 2>/dev/null | grep -q '\*'; then
    _ok "git safe.directory='*' (system)"
else
    _warn "[OPTIONAL] git safe.directory nicht system-weit gesetzt – evtl. 'dubious ownership'-Fehler in Subrepositories"
fi

# user.name / user.email
git_name=$(git config --global user.name 2>/dev/null || true)
git_email=$(git config --global user.email 2>/dev/null || true)
if [ -n "$git_name" ]; then
    _ok "git user.name='$git_name'"
else
    _warn "[OPTIONAL] git user.name nicht gesetzt – Commits ohne Autor-Namen"
fi
if [ -n "$git_email" ]; then
    _ok "git user.email='$git_email'"
else
    _warn "[OPTIONAL] git user.email nicht gesetzt"
fi

# ===========================================================================
# 8. Mounts & Laufzeit-Ressourcen
# ===========================================================================
_head "8. Mounts & Laufzeit-Ressourcen [OPTIONAL]"

# D-Bus System-Bus (für BlueZ / bthome-logger)
if [ -S /run/dbus/system_bus_socket ]; then
    _ok "D-Bus System-Bus-Socket vorhanden (/run/dbus/system_bus_socket)"
else
    _warn "[OPTIONAL] /run/dbus/system_bus_socket fehlt – BLE-Tools (bthome-logger) nicht nutzbar"
fi

# USB-Geräte (J-Link / nRF-DK)
jlink_devs=$(find /dev -name "ttyACM*" -o -name "ttyUSB*" 2>/dev/null | wc -l)
if [ "$jlink_devs" -gt 0 ]; then
    _ok "$jlink_devs serielles USB-Gerät(e) unter /dev (ttyACM*/ttyUSB*) sichtbar"
else
    _warn "[OPTIONAL] Keine seriellen USB-Geräte gefunden – J-Link/DK nicht angeschlossen oder kein Bind-Mount"
fi

# Festplattenplatz im West-Workspace
avail_kb=$(df -k "${WEST_WS:-/workspaces}" 2>/dev/null | tail -1 | awk '{print $4}')
avail_gb=$(( ${avail_kb:-0} / 1024 / 1024 ))
if [ "${avail_kb:-0}" -gt $((5 * 1024 * 1024)) ]; then
    _ok "Freier Speicher im Workspace: ${avail_gb} GB"
else
    _warn "[OPTIONAL] Wenig freier Speicher: ${avail_gb} GB – west update benötigt ~3–5 GB"
fi

# ===========================================================================
# 9. Projekt-spezifische Tools
# ===========================================================================
_head "9. Projekt-Tools [OPTIONAL]"

# bthome-logger (uv tool)
if uvx bthome-logger --version &>/dev/null 2>&1 \
   || uv tool list 2>/dev/null | grep -q bthome-logger; then
    _ok "bthome-logger installiert (uv tool)"
else
    _warn "[OPTIONAL] bthome-logger nicht installiert – 'uv tool install bthome-logger' ausführen"
fi

# ppk_analysis.py
PPK_SCRIPT="$(dirname "$(dirname "$(realpath "${BASH_SOURCE[0]}")")")/scripts/ppk_analysis.py"
if [ -f "$PPK_SCRIPT" ]; then
    _ok "scripts/ppk_analysis.py vorhanden"
else
    _warn "[OPTIONAL] scripts/ppk_analysis.py nicht gefunden"
fi

# Makefile
MAKEFILE="$(dirname "$(dirname "$(realpath "${BASH_SOURCE[0]}")")")/Makefile"
if [ -f "$MAKEFILE" ]; then
    _ok "Makefile vorhanden"
else
    _warn "[OPTIONAL] Makefile nicht gefunden"
fi

# ===========================================================================
# Zusammenfassung
# ===========================================================================
printf "\n${BOLD}══════════════════════════════════════════════════════════════════════════${RESET}\n"
printf "${BOLD}  Ergebnis:  "
printf "${GREEN}%d ✔ OK${RESET}   " "$PASS"
printf "${YELLOW}%d ⚠ Warnung(en)${RESET}   " "$WARN"
printf "${RED}%d ✘ Fehler${RESET}\n" "$FAIL"
printf "${BOLD}══════════════════════════════════════════════════════════════════════════${RESET}\n\n"

if [ "$FAIL" -gt 0 ]; then
    printf "${RED}${BOLD}FEHLER: %d Pflicht-Check(s) fehlgeschlagen.${RESET}\n" "$FAIL"
    printf "  Bitte die Punkte oben beheben und dann erneut prüfen:\n"
    printf "  ${CYAN}bash .devcontainer/check-prerequisites.sh${RESET}\n\n"
    exit 1
elif [ "$WARN" -gt 0 ]; then
    printf "${YELLOW}${BOLD}WARNUNG: %d optionale(r) Check(s) nicht erfüllt.${RESET}\n" "$WARN"
    printf "  Der Build sollte trotzdem funktionieren.\n\n"
    exit 0
else
    printf "${GREEN}${BOLD}Alle Checks bestanden. Der Devcontainer ist vollständig eingerichtet.${RESET}\n\n"
    exit 0
fi
