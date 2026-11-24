#!/usr/bin/env bash
# Clothbound installer for Arch Linux and Omarchy.
#
#   curl -fsSL https://clothbound.app/install | bash
#
# x86_64:  downloads the release package, verifies it against the release's
#          SHA256SUMS, and installs it with pacman.
# aarch64: builds the same release from source with makepkg (no prebuilt yet).
#
# Environment:
#   CLOTHBOUND_VERSION=0.4.0   install a specific release instead of the latest
#   CLOTHBOUND_DRY_RUN=1       download and verify, but install nothing
#
# Everything is wrapped in main() so a partially downloaded script never runs.

main() {
  set -euo pipefail

  local repo="orcawhisperer/clothbound"
  local dry="${CLOTHBOUND_DRY_RUN:-0}"

  say()  { printf '\033[1m==>\033[0m %s\n' "$*"; }
  die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }
  need() { command -v "$1" >/dev/null 2>&1 || die "$1 is required but not installed."; }

  command -v pacman >/dev/null 2>&1 ||
    die "Clothbound currently ships for Arch Linux and Arch-based systems (like Omarchy). See https://github.com/$repo for building elsewhere."
  need curl
  need tar
  need sha256sum

  local version="${CLOTHBOUND_VERSION:-}"
  if [ -z "$version" ]; then
    version=$(curl -fsSL "https://api.github.com/repos/$repo/releases/latest" |
      sed -n 's/.*"tag_name": *"v\{0,1\}\([^"]*\)".*/\1/p' | head -n 1)
    [ -n "$version" ] || die "Could not find the latest release. Set CLOTHBOUND_VERSION and retry."
  fi
  version="${version#v}"

  local arch
  arch="${CLOTHBOUND_ARCH:-$(uname -m)}"
  local work
  work=$(mktemp -d)
  # Expand now: the EXIT trap runs after main's locals are gone.
  # shellcheck disable=SC2064
  trap "rm -rf '$work'" EXIT
  cd "$work"

  local base="https://github.com/$repo/releases/download/v$version"

  case "$arch" in
    x86_64)
      local pkg="clothbound-$version-1-x86_64.pkg.tar.zst"
      say "Downloading Clothbound $version for x86_64"
      curl -fL --progress-bar -o "$pkg" "$base/$pkg" || die "Clothbound $version has no x86_64 package on GitHub Releases."
      curl -fsSL -o SHA256SUMS "$base/SHA256SUMS"
      say "Verifying checksum"
      grep -F " $pkg" SHA256SUMS | sha256sum -c - >/dev/null || die "Checksum mismatch for $pkg."
      if [ "$dry" = 1 ]; then say "Dry run: would run sudo pacman -U --needed $pkg"; return; fi
      say "Installing (pacman will ask for your password)"
      sudo pacman -U --needed "./$pkg" </dev/tty
      ;;
    aarch64)
      [ "$(id -u)" -ne 0 ] || die "Run this installer as a normal user (makepkg cannot run as root)."
      say "No prebuilt aarch64 package yet; building Clothbound $version from source"
      curl -fL --progress-bar -o src.tar.gz "https://github.com/$repo/archive/refs/tags/v$version.tar.gz" || die "Clothbound $version was not found on GitHub."
      tar -xzf src.tar.gz
      cd "clothbound-$version/pkgbuild"
      if [ "$dry" = 1 ]; then say "Dry run: would run makepkg -si in $PWD"; return; fi
      if ! pacman -Qg base-devel >/dev/null 2>&1; then
        say "Installing build tools (pacman will ask for your password)"
        sudo pacman -S --needed base-devel </dev/tty
      fi
      say "Building (this takes a few minutes)"
      makepkg -si --needed </dev/tty
      ;;
    *)
      die "Unsupported architecture: $arch (x86_64 and aarch64 are supported)."
      ;;
  esac

  say "Clothbound $version is installed. Open it from the app launcher, or run: clothbound"
}

main "$@"
