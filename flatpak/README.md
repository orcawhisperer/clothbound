# Clothbound Flatpak (`app.clothbound.Reader`)

Universal Linux Flatpak packaging for Ubuntu, Fedora, Debian, Linux Mint, Pop!_OS, openSUSE, NixOS, SteamOS/Bazzite, and any other Flatpak-enabled Linux desktop (`x86_64` and `aarch64`).

## Install from a GitHub Release bundle

```sh
flatpak remote-add --if-not-exists --user flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install --user -y flathub org.kde.Platform//6.10
flatpak install --user -y ./clothbound-<version>-x86_64.flatpak
flatpak run app.clothbound.Reader
```

On GNOME Wayland desktops (Ubuntu, Fedora, Debian), install the Adwaita decoration extension for native light/dark titlebars:

```sh
flatpak install --user -y flathub org.kde.WaylandDecoration.QAdwaitaDecorations//6.10
```

## Run the standalone local MCP server (`clothbound-mcp`)

The Flatpak ships both `clothbound` and the WebEngine-free `clothbound-mcp` binary:

```sh
flatpak run --command=clothbound-mcp app.clothbound.Reader
```

**Settings › Intelligence › Copy MCP config** automatically detects the Flatpak sandbox and copies the matching `flatpak run --command=clothbound-mcp app.clothbound.Reader` JSON block for Antigravity, Claude Code, Cursor, and Codex.

## Build locally

Requires `flatpak` and `flatpak-builder` with Flathub configured:

```sh
flatpak remote-add --if-not-exists --user flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak-builder --user --install --force-clean --install-deps-from=flathub \
  build-flatpak flatpak/app.clothbound.Reader.yml
```

## Validate against Flathub rules

```sh
flatpak install --user flathub org.flatpak.Builder
flatpak run --command=flatpak-builder-lint org.flatpak.Builder appstream flatpak/app.clothbound.Reader.metainfo.xml
flatpak run --command=flatpak-builder-lint org.flatpak.Builder manifest flatpak/app.clothbound.Reader.yml
```

## Submitting to Flathub

For the upstream `flathub/flathub` PR, replace the local `sources:` block in `app.clothbound.Reader.yml` with the tagged GitHub release archive (`type: archive`, `url`, `sha256` from `SHA256SUMS`), copy `flathub.json`, and publish the Flathub domain verification token at `https://clothbound.app/.well-known/org.flathub.VerifiedApps.txt`.
