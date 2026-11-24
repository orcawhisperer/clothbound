import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const siteDir = path.dirname(fileURLToPath(import.meta.url));
const repoDir = path.resolve(siteDir, "..");
const distDir = path.join(siteDir, "dist");
const distReadDir = path.join(distDir, "read");
const distFontsDir = path.join(distDir, "fonts");

fs.rmSync(distDir, { recursive: true, force: true });
fs.mkdirSync(distReadDir, { recursive: true });
fs.mkdirSync(distFontsDir, { recursive: true });

// 1. Copy and adapt index.html so /prototype and /read resolve cleanly on clothbound.app
const indexSrc = fs.readFileSync(path.join(siteDir, "index.html"), "utf8");
const indexOut = indexSrc
  .replaceAll('href="../docs/clothbound-prototype.html"', 'href="/prototype"')
  .replaceAll('href="./read.html"', 'href="/read"');
fs.writeFileSync(path.join(distDir, "index.html"), indexOut);

// 2. Copy favicon and interactive prototype
fs.copyFileSync(
  path.join(siteDir, "favicon.svg"),
  path.join(distDir, "favicon.svg")
);

const protoPath = path.join(repoDir, "docs", "clothbound-prototype.html");
if (!fs.existsSync(protoPath)) {
  throw new Error(`Missing prototype file: ${protoPath}`);
}
fs.copyFileSync(protoPath, path.join(distDir, "prototype.html"));

// 3. Copy self-hosted Literata & Inter fonts from native app source
const fontsToCopy = ["Literata.ttf", "Literata-Italic.ttf", "Inter.ttf"];
for (const fontFile of fontsToCopy) {
  const srcFont = path.join(repoDir, "src", "fonts", fontFile);
  if (!fs.existsSync(srcFont)) {
    throw new Error(`Missing font file: ${srcFont}`);
  }
  const destFont = path.join(distFontsDir, fontFile);
  fs.copyFileSync(srcFont, destFont);
  if (fs.statSync(destFont).size === 0) {
    throw new Error(`Empty font file copied: ${destFont}`);
  }
}

// 4. Copy clothbound.app/read web reader HTML, JS, and CSS
const readHtmlSrc = path.join(siteDir, "read.html");
if (!fs.existsSync(readHtmlSrc)) {
  throw new Error(`Missing web reader entry: ${readHtmlSrc}`);
}
fs.copyFileSync(readHtmlSrc, path.join(distDir, "read.html"));

const readSrcDir = path.join(siteDir, "read");
if (!fs.existsSync(readSrcDir)) {
  throw new Error(`Missing web reader asset directory: ${readSrcDir}`);
}
const readAssets = fs
  .readdirSync(readSrcDir)
  .filter((name) => /\.(?:js|css)$/i.test(name))
  .sort();

if (readAssets.length === 0) {
  throw new Error(`No web reader JS/CSS assets found in ${readSrcDir}`);
}
for (const asset of readAssets) {
  fs.copyFileSync(
    path.join(readSrcDir, asset),
    path.join(distReadDir, asset)
  );
}

// 5. Static security & caching headers for Cloudflare Workers Static Assets
const headers = `/*
  X-Content-Type-Options: nosniff
  X-Frame-Options: SAMEORIGIN
  Referrer-Policy: strict-origin-when-cross-origin
  Permissions-Policy: camera=(), microphone=(), geolocation=()

/favicon.svg
  Cache-Control: public, max-age=86400

/fonts/*
  Cache-Control: public, max-age=31536000, immutable

/read/*
  Cache-Control: public, max-age=3600, must-revalidate
`;
fs.writeFileSync(path.join(distDir, "_headers"), headers);

// 6. Validate built HTML, JS, and CSS assets (zero stale brand refs, zero external CDNs, valid JS)
const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "clothbound-site-check-"));
try {
  for (const name of ["index.html", "prototype.html", "read.html"]) {
    const html = fs.readFileSync(path.join(distDir, name), "utf8");
    if (/\bdog[-]?ear\b/i.test(html)) {
      throw new Error(`Stale brand string detected in ${name}`);
    }
    if (/<(?:script|link|img)[^>]+(?:src|href)=["']https?:\/\//i.test(html)) {
      throw new Error(`External CDN resource detected in ${name}`);
    }
    const scripts = [
      ...html.matchAll(/<script\b([^>]*)>([\s\S]*?)<\/script>/gi),
    ];
    scripts.forEach((m, idx) => {
      const attrs = m[1] || "";
      const body = m[2] || "";
      if (/\bsrc=/i.test(attrs) && body.trim() === "") return;
      const ext = /\btype=["']module["']/i.test(attrs) ? "mjs" : "js";
      const jsFile = path.join(tmpDir, `${name}.${idx}.${ext}`);
      fs.writeFileSync(jsFile, body);
      execFileSync(process.execPath, ["--check", jsFile], { stdio: "inherit" });
    });
  }

  for (const asset of readAssets) {
    const assetPath = path.join(distReadDir, asset);
    const content = fs.readFileSync(assetPath, "utf8");
    if (/\bdog[-]?ear\b/i.test(content)) {
      throw new Error(`Stale brand string detected in dist/read/${asset}`);
    }
    if (asset.endsWith(".css")) {
      if (/url\(\s*["']?https?:\/\//i.test(content) || /@import\s+["']?https?:\/\//i.test(content)) {
        throw new Error(`External URL detected in dist/read/${asset}`);
      }
    }
    if (asset.endsWith(".js")) {
      if (
        /(?:import\s+[^"']*from\s*|import\s*\(\s*|fetch\s*\(\s*)["']https?:\/\//i.test(
          content
        )
      ) {
        throw new Error(`External network/CDN reference detected in dist/read/${asset}`);
      }
      execFileSync(process.execPath, ["--check", assetPath], {
        stdio: "inherit",
      });
    }
  }
} finally {
  fs.rmSync(tmpDir, { recursive: true, force: true });
}

console.log(
  `Built clothbound.app static bundle into ${distDir} (${fontsToCopy.length} fonts, ${readAssets.length} reader assets)`
);
