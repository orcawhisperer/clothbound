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

// 2. Copy favicon, social preview card, and interactive prototype
fs.copyFileSync(
  path.join(siteDir, "favicon.svg"),
  path.join(distDir, "favicon.svg")
);

const ogCardPath = path.join(siteDir, "og-card.png");
if (!fs.existsSync(ogCardPath)) {
  throw new Error(`Missing social preview image: ${ogCardPath}`);
}
fs.copyFileSync(ogCardPath, path.join(distDir, "og-card.png"));

const protoPath = path.join(repoDir, "docs", "clothbound-prototype.html");
if (!fs.existsSync(protoPath)) {
  throw new Error(`Missing prototype file: ${protoPath}`);
}
fs.copyFileSync(protoPath, path.join(distDir, "prototype.html"));

// 2b. Minimal robots.txt and sitemap.xml for search discovery
fs.writeFileSync(
  path.join(distDir, "robots.txt"),
  "User-agent: *\nAllow: /\nSitemap: https://clothbound.app/sitemap.xml\n"
);
fs.writeFileSync(
  path.join(distDir, "sitemap.xml"),
  `<?xml version="1.0" encoding="UTF-8"?>
<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">
  <url><loc>https://clothbound.app/</loc></url>
  <url><loc>https://clothbound.app/prototype</loc></url>
</urlset>
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

/og-card.png
  Cache-Control: public, max-age=86400
`;
fs.writeFileSync(path.join(distDir, "_headers"), headers);

// 4. Validate built HTML assets (zero stale brand refs, zero external CDNs, valid JS)
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
