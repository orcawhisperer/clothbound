import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const siteDir = path.dirname(fileURLToPath(import.meta.url));
const repoDir = path.resolve(siteDir, "..");
const distDir = path.join(siteDir, "dist");

fs.rmSync(distDir, { recursive: true, force: true });
fs.mkdirSync(distDir, { recursive: true });

// 1. Copy and adapt index.html so /prototype resolves cleanly on clothbound.app
const indexSrc = fs.readFileSync(path.join(siteDir, "index.html"), "utf8");
const indexOut = indexSrc.replaceAll(
  'href="../docs/clothbound-prototype.html"',
  'href="/prototype"'
);
fs.writeFileSync(path.join(distDir, "index.html"), indexOut);

// 2. Copy favicon, apple-touch-icon, social preview card, and interactive prototype
fs.copyFileSync(
  path.join(siteDir, "favicon.svg"),
  path.join(distDir, "favicon.svg")
);

const appleIconPath = path.join(siteDir, "apple-touch-icon.png");
if (!fs.existsSync(appleIconPath)) {
  throw new Error(`Missing apple-touch-icon image: ${appleIconPath}`);
}
fs.copyFileSync(appleIconPath, path.join(distDir, "apple-touch-icon.png"));

const ogCardPath = path.join(siteDir, "og-card.png");
if (!fs.existsSync(ogCardPath)) {
  throw new Error(`Missing social preview image: ${ogCardPath}`);
}
fs.copyFileSync(ogCardPath, path.join(distDir, "og-card.png"));

const protoPath = path.join(repoDir, "docs", "clothbound-prototype.html");
if (!fs.existsSync(protoPath)) {
  throw new Error(`Missing prototype file: ${protoPath}`);
}
const protoSeoTags = `<meta name="description" content="Explore Clothbound's interactive 18-board UI design prototype: library, zero-flash EPUB &amp; PDF reading, Study Companion, PDF Snip &amp; Peek, Narrator, Settings, and Local MCP.">
<meta name="robots" content="index,follow,max-image-preview:large">
<meta property="og:type" content="website">
<meta property="og:site_name" content="Clothbound">
<meta property="og:title" content="Clothbound — Interactive 18-Board Design Prototype">
<meta property="og:description" content="Explore all 18 interactive screens of Clothbound — a native Qt6 EPUB &amp; PDF reader for Linux and Omarchy.">
<meta property="og:url" content="https://clothbound.app/prototype">
<meta property="og:image" content="https://clothbound.app/og-card.png">
<meta property="og:image:alt" content="Clothbound v0.5.0 — Native Qt6 EPUB and PDF reader for Linux and Omarchy">
<meta name="twitter:card" content="summary_large_image">
<meta name="twitter:title" content="Clothbound — Interactive 18-Board Design Prototype">
<meta name="twitter:description" content="Explore all 18 interactive screens of Clothbound — a native Qt6 EPUB &amp; PDF reader for Linux and Omarchy.">
<meta name="twitter:image" content="https://clothbound.app/og-card.png">
<link rel="canonical" href="https://clothbound.app/prototype">
<link rel="icon" type="image/svg+xml" href="/favicon.svg">
<link rel="apple-touch-icon" sizes="180x180" href="/apple-touch-icon.png">
<title>`;
const protoSrc = fs.readFileSync(protoPath, "utf8");
const protoOut = protoSrc.replace("<title>", protoSeoTags);
fs.writeFileSync(path.join(distDir, "prototype.html"), protoOut);

// 2a. One-line installer, served at /install and /install.sh
const installSrc = fs.readFileSync(path.join(siteDir, "install.sh"));
fs.writeFileSync(path.join(distDir, "install"), installSrc);
fs.writeFileSync(path.join(distDir, "install.sh"), installSrc);

// 2b. robots.txt, sitemap.xml, and llms.txt for search engines and AI discovery
const lastmod = "2026-11-25";
fs.writeFileSync(
  path.join(distDir, "robots.txt"),
  "User-agent: *\nAllow: /\nSitemap: https://clothbound.app/sitemap.xml\n"
);
fs.writeFileSync(
  path.join(distDir, "sitemap.xml"),
  `<?xml version="1.0" encoding="UTF-8"?>
<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">
  <url>
    <loc>https://clothbound.app/</loc>
    <lastmod>${lastmod}</lastmod>
    <changefreq>weekly</changefreq>
    <priority>1.0</priority>
  </url>
  <url>
    <loc>https://clothbound.app/prototype</loc>
    <lastmod>${lastmod}</lastmod>
    <changefreq>monthly</changefreq>
    <priority>0.8</priority>
  </url>
</urlset>
`
);
fs.writeFileSync(
  path.join(distDir, "llms.txt"),
  `# Clothbound

> A calm, local-first native Qt 6 EPUB and PDF reader for Linux, Wayland, and Omarchy (v0.5.0, MIT licensed).

- Website: https://clothbound.app
- Interactive 18-board UI Prototype: https://clothbound.app/prototype
- Source & Releases: https://github.com/orcawhisperer/clothbound
- One-line Arch / Omarchy Installer: \`curl -fsSL https://clothbound.app/install | bash\` (inspect at https://clothbound.app/install.sh)

## Core Capabilities
- **Library & Instant Search (\`Ctrl+K\`)**: Local watched folders (files never modified), edition grouping, collections, local SQLite FTS5 full-text search, optional \`int8\`-quantized SIMD semantic search, off-UI-thread Command Palette, W3C text-quote anchored highlights/notes, Obsidian/Readwise Markdown export.
- **Online-to-Offline (\`Ctrl+Shift+U\` / \`Ctrl+Shift+O\`)**: Save web articles offline as EPUBs on an Articles shelf; fetch arXiv, alphaXiv, Hugging Face Papers, and direct EPUB/PDF links; browse cached OPDS 1.2 & 2.0 catalogs (Standard Ebooks, Project Gutenberg, Open Library, arXiv, Calibre-Web, Kavita, Komga).
- **Academic PDF Craft**: Hue-preserving theme-recolored pages with white-margin cropping, tiled viewport rendering & cursor-focal zoom, preset view modes (\`0\` Fit page, \`W\` Fit width, \`1\` Actual), Visual Snip & Explain (\`X\`), Zero-Jump Quick-Look citation/figure peek cards, and automatic arXiv/DOI BibTeX & APA copy.
- **Read-Aloud (\`P\`)**: Built-in offline neural voices (Kokoro, Pocket TTS, Piper across 42 languages), optional Gemini/Studio voices, 44px/86px floating player with continuous Library mini-player, sleep timer, and M4B audiobook export.
- **Story & Study Companion (\`C\`)**: Spoiler-free Story mode and Study mode (Brief, Glossary, Smart Scope, offline LaTeX math) with smooth GPU-lockstep docking via local Ollama or your own Gemini key.
- **Standalone Local MCP Server (\`clothbound-mcp\`)**: WebEngine-free read-only stdio JSON-RPC 2.0 server (~31 ms startup) exposing \`list_books\`, \`search_library\`, \`search_passages\`, \`get_book_outline\`, \`read_passage\`, and \`list_marks\`.
- **Desktop Integration**: Follows Omarchy palettes, 50 fully translated interface languages, and Hyprland corner rounding live (plus in-app Light/Dark and Rounded/Sharp overrides).
`
);

// 3. Static security & caching headers for Cloudflare Workers Static Assets
const headers = `/*
  X-Content-Type-Options: nosniff
  X-Frame-Options: SAMEORIGIN
  Referrer-Policy: strict-origin-when-cross-origin
  Permissions-Policy: camera=(), microphone=(), geolocation=()

/favicon.svg
  Cache-Control: public, max-age=86400

/apple-touch-icon.png
  Cache-Control: public, max-age=86400

/og-card.png
  Cache-Control: public, max-age=86400

/install
  Content-Type: text/plain; charset=utf-8
  Cache-Control: public, max-age=300

/install.sh
  Content-Type: text/plain; charset=utf-8
  Cache-Control: public, max-age=300

/llms.txt
  Content-Type: text/plain; charset=utf-8
  Cache-Control: public, max-age=3600
`;
fs.writeFileSync(path.join(distDir, "_headers"), headers);

// 4. Validate built HTML assets (zero stale brand refs, zero external CDNs, valid JS & JSON-LD)
const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "clothbound-site-check-"));
try {
  for (const name of ["index.html", "prototype.html"]) {
    const html = fs.readFileSync(path.join(distDir, name), "utf8");
    if (/\bdog[-]?ear\b/i.test(html)) {
      throw new Error(`Stale brand string detected in ${name}`);
    }
    if (/<(?:script|img)[^>]+src=["']https?:\/\//i.test(html) ||
        /<link(?![^>]*rel=["']canonical["'])[^>]+href=["']https?:\/\//i.test(html)) {
      throw new Error(`External CDN resource detected in ${name}`);
    }
    const scripts = [
      ...html.matchAll(/<script\b([^>]*)>([\s\S]*?)<\/script>/gi),
    ];
    scripts.forEach((m, idx) => {
      const attrs = m[1] || "";
      const body = m[2] || "";
      if (/\bsrc=/i.test(attrs) && body.trim() === "") return;
      if (/\btype=["']application\/ld\+json["']/i.test(attrs)) {
        JSON.parse(body);
        return;
      }
      const ext = /\btype=["']module["']/i.test(attrs) ? "mjs" : "js";
      const jsFile = path.join(tmpDir, `${name}.${idx}.${ext}`);
      fs.writeFileSync(jsFile, body);
      execFileSync(process.execPath, ["--check", jsFile], { stdio: "inherit" });
    });
  }
} finally {
  fs.rmSync(tmpDir, { recursive: true, force: true });
}

console.log(`Built clothbound.app static bundle into ${distDir}`);
