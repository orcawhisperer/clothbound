#!/usr/bin/env bash
# Clothbound installer for Arch Linux and Omarchy.
#
#   curl -fsSL https://clothbound.app/install | bash
#
# x86_64:  downloads the release package, verifies it against the release's
#          SHA256SUMS, and installs it with pacman.
# aarch64: downloads the tagged source archive, verifies it against the
#          release's SHA256SUMS, and builds from source with makepkg.
#
# Environment:
#   CLOTHBOUND_VERSION=0.4.1   install a specific release instead of the latest
#   CLOTHBOUND_DRY_RUN=1       download and verify, but install nothing
#
# Everything is wrapped in main() so a partially downloaded script never runs.

_clothbound_work=""

main() {
  set -euo pipefail

  local repo="orcawhisperer/clothbound"
  local dry="${CLOTHBOUND_DRY_RUN:-0}"

  say()  { printf '\033[1m==>\033[0m %s\n' "$*"; }
  die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }
  need() { command -v "$1" >/dev/null 2>&1 || die "$1 is required but not installed."; }

  verify_sha256() {
    local file="$1"
    shift
    local expected=""
    local candidate
    for candidate in "$file" "$@"; do
      expected=$(awk -v f="$candidate" 'NF >= 2 && ($2 == f || $2 == ("*" f)) { print $1; exit }' SHA256SUMS)
      [ -n "$expected" ] && break
    done
    [ -n "$expected" ] || die "No SHA256SUMS entry found for $file."
    printf '%s  %s\n' "$expected" "$file" | sha256sum -c - >/dev/null || die "Checksum mismatch for $file."
  }

  command -v pacman >/dev/null 2>&1 ||
    die "Clothbound currently ships for Arch Linux and Arch-based systems (like Omarchy). See https://github.com/$repo for building elsewhere."
  need curl
  need tar
  need sha256sum
  need awk

  local version="${CLOTHBOUND_VERSION:-}"
  if [ -z "$version" ]; then
    version=$(curl -fsSL "https://api.github.com/repos/$repo/releases/latest" |
      sed -n 's/.*"tag_name": *"v\{0,1\}\([^"]*\)".*/\1/p' | head -n 1)
    [ -n "$version" ] || die "Could not find the latest release. Set CLOTHBOUND_VERSION and retry."
  fi
  local version_re='^v?[0-9]+\.[0-9]+\.[0-9]+([-.][A-Za-z0-9.]+)?$'
  [[ "$version" =~ $version_re ]] || die "Invalid version format: $version"
  version="${version#v}"

  local arch
  arch="${CLOTHBOUND_ARCH:-$(uname -m)}"
  _clothbound_work=$(mktemp -d)
  trap '[ -n "${_clothbound_work:-}" ] && rm -rf -- "$_clothbound_work"' EXIT
  cd "$_clothbound_work"

  local base="https://github.com/$repo/releases/download/v$version"

  case "$arch" in
    x86_64)
      local pkg="clothbound-$version-1-x86_64.pkg.tar.zst"
      say "Downloading Clothbound $version for x86_64"
      curl -fL --progress-bar -o "$pkg" "$base/$pkg" || die "Clothbound $version has no x86_64 package on GitHub Releases."
      curl -fsSL -o SHA256SUMS "$base/SHA256SUMS" || die "Could not download SHA256SUMS for Clothbound $version."
      say "Verifying checksum"
      verify_sha256 "$pkg"
      if [ "$dry" = 1 ]; then say "Dry run: would run sudo pacman -U --needed $pkg"; return; fi
      say "Installing (pacman will ask for your password)"
      sudo pacman -U --needed "./$pkg" </dev/tty
      ;;
    aarch64)
      [ "$(id -u)" -ne 0 ] || die "Run this installer as a normal user (makepkg cannot run as root)."
      local src="clothbound-$version.tar.gz"
      say "No prebuilt aarch64 package yet; building Clothbound $version from source"
      curl -fL --progress-bar -o "$src" "https://github.com/$repo/archive/refs/tags/v$version.tar.gz" || die "Clothbound $version was not found on GitHub."
      curl -fsSL -o SHA256SUMS "$base/SHA256SUMS" || die "Could not download SHA256SUMS for Clothbound $version."
      say "Verifying checksum"
      verify_sha256 "$src" "v$version.tar.gz" "src.tar.gz"
      tar -xzf "$src"
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
