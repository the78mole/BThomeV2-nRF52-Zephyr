# Devcontainer für das nRF Connect SDK (NCS v2.6.0)
# Basis: Ubuntu 26.04 LTS
# Python-Verwaltung: uv (https://docs.astral.sh/uv/)
# Toolchain: Zephyr SDK 0.16.5 – ARM only (arm-zephyr-eabi)

FROM ubuntu:26.04

ARG DEBIAN_FRONTEND=noninteractive
ARG ZEPHYR_SDK_VERSION=0.16.5

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
        ca-certificates \
        sudo \
    && rm -rf /var/lib/apt/lists/*

# ---------------------------------------------------------------------------
# uv installieren (Astral – schneller Python-Paketmanager)
# https://docs.astral.sh/uv/getting-started/installation/
# ---------------------------------------------------------------------------
RUN curl -LsSf https://astral.sh/uv/install.sh | sh

ENV PATH="/root/.local/bin:$PATH"

# Tool-Binaries systemweit in /usr/local/bin ablegen,
# damit sie für alle Nutzer (auch non-root via sudo) erreichbar sind.
ENV UV_TOOL_BIN_DIR=/usr/local/bin

# ---------------------------------------------------------------------------
# west CLI via uv tool install
# ---------------------------------------------------------------------------
RUN uv tool install west

# ---------------------------------------------------------------------------
# Virtuelle Python-Umgebung für NCS-Build-Skripte
# (cmake/Zephyr-Skripte nutzen VIRTUAL_ENV automatisch)
# ---------------------------------------------------------------------------
RUN uv venv /opt/ncs/venv

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
# ---------------------------------------------------------------------------
RUN git config --global safe.directory '*'

WORKDIR /workspaces
