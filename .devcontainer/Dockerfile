# Devcontainer für das nRF Connect SDK (NCS v2.6.0)
# Basis: Ubuntu 26.04 LTS
# Python-Verwaltung: uv (https://docs.astral.sh/uv/)
# Toolchain: Zephyr SDK 0.16.5 – ARM only (arm-zephyr-eabi)

FROM ubuntu:26.04

ARG DEBIAN_FRONTEND=noninteractive
ARG ZEPHYR_SDK_VERSION=0.16.5

# ---------------------------------------------------------------------------
# Ubuntu 26.04 ships with a built-in 'ubuntu' user at UID/GID 1000.
# The common-utils devcontainer feature also creates a user at UID/GID 1000
# ('vscode'). Remove the existing ubuntu user/group so UID/GID 1000 is free
# for the feature to use.  Note: GID 65534 is already taken by 'nogroup' in
# Ubuntu 26.04, so groupmod -g 65534 would fail – deletion is safer.
# ---------------------------------------------------------------------------
RUN if getent passwd 1000 >/dev/null 2>&1; then \
        userdel "$(getent passwd 1000 | cut -d: -f1)"; \
    fi \
    && if getent group 1000 >/dev/null 2>&1; then \
        groupdel "$(getent group 1000 | cut -d: -f1)"; \
    fi

# ---------------------------------------------------------------------------
# Locale
# ---------------------------------------------------------------------------
RUN apt-get update && apt-get install -y --no-install-recommends \
        locales \
    && locale-gen en_US.UTF-8 \
    && update-locale LANG=en_US.UTF-8 \
    && rm -rf /var/lib/apt/lists/*

ENV LANG=en_US.UTF-8
ENV LC_ALL=en_US.UTF-8

# ---------------------------------------------------------------------------
# System-Abhängigkeiten für NCS / Zephyr
# Versionsanforderungen NCS v2.6.0:
#   cmake ≥ 3.21 | ninja ≥ 1.10.2 | dtc ≥ 1.4.7 | gperf ≥ 3.1
# ---------------------------------------------------------------------------
RUN apt-get update && apt-get install -y --no-install-recommends \
        git \
        cmake \
        ninja-build \
        gperf \
        ccache \
        dfu-util \
        device-tree-compiler \
        wget \
        curl \
        xz-utils \
        file \
        make \
        gcc \
        gcc-multilib \
        g++-multilib \
        libsdl2-dev \
        libmagic1 \
        python3 \
        python3-dev \
        python-is-python3 \
        ca-certificates \
        sudo \
    && rm -rf /var/lib/apt/lists/*

# ---------------------------------------------------------------------------
# uv installieren (Astral – schneller Python-Paketmanager)
# Systemweit nach /usr/local/bin, damit der non-root user (vscode, UID 1000)
# uv ohne PATH-Anpassung nutzen kann.
# https://docs.astral.sh/uv/getting-started/installation/
# ---------------------------------------------------------------------------
RUN curl -LsSf https://astral.sh/uv/install.sh | sh \
    && mv /root/.local/bin/uv  /usr/local/bin/uv \
    && mv /root/.local/bin/uvx /usr/local/bin/uvx \
    && rm -rf /root/.local



# ---------------------------------------------------------------------------
# Virtuelle Python-Umgebung für NCS-Build-Skripte
# (cmake/Zephyr-Skripte nutzen VIRTUAL_ENV automatisch)
# ---------------------------------------------------------------------------
RUN uv venv /opt/ncs/venv

# ---------------------------------------------------------------------------
# west direkt in die venv installieren:
#   - west CLI erreichbar über /opt/ncs/venv/bin/west (liegt im PATH)
#   - west Python-APIs aus der venv importierbar (nötig für NCS-Build-Skripte)
#   - uv tool install west entfällt (erzeugt isolierte Umgebung ohne venv-Zugriff)
# ---------------------------------------------------------------------------
RUN uv pip install --python /opt/ncs/venv west

ENV VIRTUAL_ENV=/opt/ncs/venv
ENV PATH="/opt/ncs/venv/bin:$PATH"

# ---------------------------------------------------------------------------
# Zephyr SDK – minimales Paket + ARM-Toolchain (arm-zephyr-eabi)
# Vollständige Toolchain-Liste: https://github.com/zephyrproject-rtos/sdk-ng
# ---------------------------------------------------------------------------
WORKDIR /tmp

RUN set -e \
    && wget -q \
        "https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${ZEPHYR_SDK_VERSION}/zephyr-sdk-${ZEPHYR_SDK_VERSION}_linux-x86_64_minimal.tar.xz" \
    && tar xf "zephyr-sdk-${ZEPHYR_SDK_VERSION}_linux-x86_64_minimal.tar.xz" -C /opt \
    && rm    "zephyr-sdk-${ZEPHYR_SDK_VERSION}_linux-x86_64_minimal.tar.xz" \
    && wget -q \
        "https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${ZEPHYR_SDK_VERSION}/toolchain_linux-x86_64_arm-zephyr-eabi.tar.xz" \
    && tar xf "toolchain_linux-x86_64_arm-zephyr-eabi.tar.xz" -C "/opt/zephyr-sdk-${ZEPHYR_SDK_VERSION}/" \
    && rm    "toolchain_linux-x86_64_arm-zephyr-eabi.tar.xz" \
    && "/opt/zephyr-sdk-${ZEPHYR_SDK_VERSION}/setup.sh" -c

ENV ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-${ZEPHYR_SDK_VERSION}
ENV ZEPHYR_TOOLCHAIN_VARIANT=zephyr

# ---------------------------------------------------------------------------
# Sicherheit: gemountete Verzeichnisse immer als git-sicher einstufen
# --system schreibt nach /etc/gitconfig – gilt für alle Benutzer im Container
# ---------------------------------------------------------------------------
RUN git config --system safe.directory '*'

# ---------------------------------------------------------------------------
# Berechtigungen für den non-root user (vscode, UID 1000)
#
# common-utils erstellt im devcontainer-Build einen Benutzer 'vscode' mit
# UID/GID 1000. Damit dieser die virtuelle Python-Umgebung beschreiben
# (west pip-Abhängigkeiten) und in /workspaces arbeiten kann, wird hier
# die Eigentümerschaft gesetzt.
#
# Die Toolchain unter /opt/zephyr-sdk-* wird nur gelesen → kein chown nötig,
# da alle Binaries world-executable sind (setup.sh -c registriert CMake
# system-weit unter /usr/share/cmake).
# ---------------------------------------------------------------------------
RUN chown -R 1000:1000 /opt/ncs

RUN mkdir -p /workspaces && chown 1000:1000 /workspaces
WORKDIR /workspaces
