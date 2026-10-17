# SPDX-License-Identifier: MIT
# Linux development image for LegacyRecomp (see docs/development.md).
FROM archlinux:latest
RUN pacman -Sy --noconfirm --needed clang cmake ninja sdl2 pkgconf git python binutils rsync \
    && pacman -Scc --noconfirm
WORKDIR /work
