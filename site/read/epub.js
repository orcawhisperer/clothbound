/**
 * Clothbound Web — Zero-Dependency Client-Side EPUB 2 & EPUB 3 Engine
 *
 * Pure ES module:
 *  - ZIP central-directory reader (stored method 0 & deflate method 8, ZIP64 sizes,
 *    case-insensitive path resolution) using native DecompressionStream('deflate-raw')
 *    with a self-contained RFC 1951 pure-JS inflateRaw fallback.
 *  - Parses META-INF/container.xml, OPF metadata/manifest/spine/cover, EPUB 3 nav.xhtml
 *    and EPUB 2 toc.ncx.
 *  - Rewrites local chapter images & stylesheets to tracked Blob URLs (revoked on dispose).
 *  - Sanitizes chapter XHTML strictly for local-only offline rendering.
 *  - Exports buildMinimalEpubBlob(...) so built-in classics & tests run through the
 *    exact same EPUB ZIP pipeline as user-imported .epub files.
 */

const utf8Decoder = new TextDecoder("utf-8", { fatal: false });
const utf8Encoder = new TextEncoder();

/* ============================================================================
   1. CRC-32 & Minimal RFC 1951 Pure-JS Inflate Fallback
   ============================================================================ */

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) {
      c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
    }
    t[i] = c >>> 0;
  }
  return t;
})();

function crc32(bytes) {
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) {
    c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  }
  return (c ^ 0xffffffff) >>> 0;
}

const LEN_BASE = [
  3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
];
const LEN_EXTRA = [
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
  3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
];
const DIST_BASE = [
  1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
  257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
];
const DIST_EXTRA = [
  0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
  7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
];
const CL_ORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];

function buildHuffmanTree(lengths) {
  let maxBits = 0;
  for (let i = 0; i < lengths.length; i++) {
    if (lengths[i] > maxBits) maxBits = lengths[i];
  }
  if (maxBits === 0) {
    return { counts: new Int32Array(16), symbols: new Int32Array(0), maxBits: 0 };
  }
  const counts = new Int32Array(maxBits + 1);
  for (let i = 0; i < lengths.length; i++) {
    if (lengths[i] > 0) counts[lengths[i]]++;
  }
  const offsets = new Int32Array(maxBits + 1);
  let sum = 0;
  for (let b = 1; b <= maxBits; b++) {
    offsets[b] = sum;
    sum += counts[b];
  }
  const symbols = new Int32Array(sum);
  for (let i = 0; i < lengths.length; i++) {
    const len = lengths[i];
    if (len > 0) {
      symbols[offsets[len]++] = i;
    }
  }
  return { counts, symbols, maxBits };
}

const FIXED_LIT_TREE = (() => {
  const lens = new Uint8Array(288);
  lens.fill(8, 0, 144);
  lens.fill(9, 144, 256);
  lens.fill(7, 256, 280);
  lens.fill(8, 280, 288);
  return buildHuffmanTree(lens);
})();

const FIXED_DIST_TREE = (() => {
  const lens = new Uint8Array(32);
  lens.fill(5);
  return buildHuffmanTree(lens);
})();

/**
 * Pure-JS RFC 1951 raw deflate decompressor (fallback when DecompressionStream is unavailable).
 */
export function inflateRawFallback(src, expectedSize = 0) {
  let pos = 0;
  let bitBuf = 0;
  let bitCount = 0;

  let out = new Uint8Array(Math.max(expectedSize || 0, src.length * 2, 1024));
  let outPos = 0;

  function ensureSpace(extra) {
    if (outPos + extra <= out.length) return;
    let newLen = out.length * 2;
    while (newLen < outPos + extra) newLen *= 2;
    const grown = new Uint8Array(newLen);
    grown.set(out.subarray(0, outPos));
    out = grown;
  }

  function readBits(n) {
    while (bitCount < n) {
      if (pos >= src.length) throw new Error("Unexpected end of deflate stream");
      bitBuf |= src[pos++] << bitCount;
      bitCount += 8;
    }
    const val = bitBuf & ((1 << n) - 1);
    bitBuf >>>= n;
    bitCount -= n;
    return val;
  }

  function alignByte() {
    bitBuf = 0;
    bitCount = 0;
  }

  function decodeSymbol(tree) {
    let code = 0;
    let first = 0;
    let index = 0;
    for (let len = 1; len <= tree.maxBits; len++) {
      code |= readBits(1);
      const count = tree.counts[len];
      if (code - count < first) {
        return tree.symbols[index + (code - first)];
      }
      index += count;
      first = (first + count) << 1;
      code <<= 1;
    }
    throw new Error("Invalid Huffman code in deflate stream");
  }

  let isFinal = 0;
  while (!isFinal) {
    isFinal = readBits(1);
    const btype = readBits(2);
    if (btype === 0) {
      alignByte();
      if (pos + 4 > src.length) throw new Error("Truncated stored block");
      const len = src[pos] | (src[pos + 1] << 8);
      const nlen = src[pos + 2] | (src[pos + 3] << 8);
      pos += 4;
      if ((len ^ 0xffff) !== nlen) throw new Error("Corrupted stored block length");
      if (pos + len > src.length) throw new Error("Truncated stored block data");
      ensureSpace(len);
      out.set(src.subarray(pos, pos + len), outPos);
      pos += len;
      outPos += len;
    } else if (btype === 1 || btype === 2) {
      let litTree = FIXED_LIT_TREE;
      let distTree = FIXED_DIST_TREE;
      if (btype === 2) {
        const hlit = readBits(5) + 257;
        const hdist = readBits(5) + 1;
        const hclen = readBits(4) + 4;
        const clLens = new Uint8Array(19);
        for (let i = 0; i < hclen; i++) {
          clLens[CL_ORDER[i]] = readBits(3);
        }
        const clTree = buildHuffmanTree(clLens);
        const total = hlit + hdist;
        const lens = new Uint8Array(total);
        let idx = 0;
        while (idx < total) {
          const sym = decodeSymbol(clTree);
          if (sym < 16) {
            lens[idx++] = sym;
          } else if (sym === 16) {
            if (idx === 0) throw new Error("Invalid repeat in Huffman table");
            const rep = readBits(2) + 3;
            const prev = lens[idx - 1];
            for (let r = 0; r < rep && idx < total; r++) lens[idx++] = prev;
          } else if (sym === 17) {
            const rep = readBits(3) + 3;
            for (let r = 0; r < rep && idx < total; r++) lens[idx++] = 0;
          } else if (sym === 18) {
            const rep = readBits(7) + 11;
            for (let r = 0; r < rep && idx < total; r++) lens[idx++] = 0;
          } else {
            throw new Error("Invalid code-length symbol");
          }
        }
        litTree = buildHuffmanTree(lens.subarray(0, hlit));
        distTree = buildHuffmanTree(lens.subarray(hlit, total));
      }

      while (true) {
        const sym = decodeSymbol(litTree);
        if (sym < 256) {
          ensureSpace(1);
          out[outPos++] = sym;
        } else if (sym === 256) {
          break;
        } else if (sym <= 285) {
          const lenIdx = sym - 257;
          const length = LEN_BASE[lenIdx] + (LEN_EXTRA[lenIdx] ? readBits(LEN_EXTRA[lenIdx]) : 0);
          const distSym = decodeSymbol(distTree);
          if (distSym >= 30) throw new Error("Invalid distance symbol");
          const distance = DIST_BASE[distSym] + (DIST_EXTRA[distSym] ? readBits(DIST_EXTRA[distSym]) : 0);
          if (distance > outPos) throw new Error("Deflate back-reference out of range");
          ensureSpace(length);
          for (let i = 0; i < length; i++) {
            out[outPos] = out[outPos - distance];
            outPos++;
          }
        } else {
          throw new Error("Invalid literal/length symbol");
        }
      }
    } else {
      throw new Error("Invalid deflate block type 3");
    }
  }

  return out.subarray(0, outPos);
}

async function inflateRaw(compressed, expectedSize = 0) {
  if (typeof DecompressionStream === "function") {
    try {
      const ds = new DecompressionStream("deflate-raw");
      const writer = ds.writable.getWriter();
      writer.write(compressed).catch(() => {});
      writer.close().catch(() => {});
      const resp = new Response(ds.readable);
      const ab = await resp.arrayBuffer();
      return new Uint8Array(ab);
    } catch {
      return inflateRawFallback(compressed, expectedSize);
    }
  }
  return inflateRawFallback(compressed, expectedSize);
}

/* ============================================================================
   2. Pure-JS ZIP Central-Directory Reader (Stored, Deflate, ZIP64)
   ============================================================================ */

function readU64LE(view, offset) {
  const lo = view.getUint32(offset, true);
  const hi = view.getUint32(offset + 4, true);
  return hi * 4294967296 + lo;
}

function normalizeZipPath(p) {
  let s = String(p || "").replace(/\\/g, "/");
  s = s.replace(/^\/+/, "");
  const parts = [];
  for (const part of s.split("/")) {
    if (!part || part === ".") continue;
    if (part === "..") {
      parts.pop();
    } else {
      parts.push(part);
    }
  }
  return parts.join("/");
}

function safeDecodeURI(p) {
  try {
    return decodeURIComponent(p);
  } catch {
    return p;
  }
}

export function readZip(input) {
  const bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const len = bytes.byteLength;

  if (len < 22) {
    throw new Error("File is too small to be a valid ZIP/EPUB archive.");
  }

  // Locate EOCD signature 0x06054b50 scanning backward
  const minPos = Math.max(0, len - 65557);
  let eocdPos = -1;
  for (let i = len - 22; i >= minPos; i--) {
    if (view.getUint32(i, true) === 0x06054b50) {
      const commentLen = view.getUint16(i + 20, true);
      if (i + 22 + commentLen <= len) {
        eocdPos = i;
        break;
      }
    }
  }
  if (eocdPos < 0) {
    throw new Error("Could not find ZIP End of Central Directory (not a valid EPUB).");
  }

  let entryCount = view.getUint16(eocdPos + 10, true);
  let cdSize = view.getUint32(eocdPos + 12, true);
  let cdOffset = view.getUint32(eocdPos + 16, true);

  // Check for ZIP64 EOCD Locator immediately before EOCD (20 bytes)
  if (
    eocdPos >= 20 &&
    view.getUint32(eocdPos - 20, true) === 0x07064b50
  ) {
    const zip64EocdOffset = readU64LE(view, eocdPos - 12);
    if (
      zip64EocdOffset + 56 <= len &&
      view.getUint32(zip64EocdOffset, true) === 0x06064b50
    ) {
      if (entryCount === 0xffff) entryCount = readU64LE(view, zip64EocdOffset + 32);
      if (cdSize === 0xffffffff) cdSize = readU64LE(view, zip64EocdOffset + 40);
      if (cdOffset === 0xffffffff) cdOffset = readU64LE(view, zip64EocdOffset + 48);
    }
  }

  const exactMap = new Map();
  const lowerMap = new Map();
  const entries = [];

  let cursor = cdOffset;
  const cdEnd = Math.min(len, cdOffset + cdSize);

  for (let i = 0; i < entryCount && cursor + 46 <= cdEnd; i++) {
    if (view.getUint32(cursor, true) !== 0x02014b50) break;

    const method = view.getUint16(cursor + 10, true);
    let compSize = view.getUint32(cursor + 20, true);
    let uncompSize = view.getUint32(cursor + 24, true);
    const nameLen = view.getUint16(cursor + 28, true);
    const extraLen = view.getUint16(cursor + 30, true);
    const commentLen = view.getUint16(cursor + 32, true);
    let localOffset = view.getUint32(cursor + 42, true);

    const rawName = utf8Decoder.decode(
      bytes.subarray(cursor + 46, cursor + 46 + nameLen)
    );
    const normName = normalizeZipPath(rawName);

    // Parse ZIP64 extra field (header ID 0x0001) if needed
    if (
      compSize === 0xffffffff ||
      uncompSize === 0xffffffff ||
      localOffset === 0xffffffff
    ) {
      let exPos = cursor + 46 + nameLen;
      const exEnd = exPos + extraLen;
      while (exPos + 4 <= exEnd) {
        const headerId = view.getUint16(exPos, true);
        const dataSize = view.getUint16(exPos + 2, true);
        exPos += 4;
        if (headerId === 0x0001) {
          let fieldPos = exPos;
          if (uncompSize === 0xffffffff && fieldPos + 8 <= exPos + dataSize) {
            uncompSize = readU64LE(view, fieldPos);
            fieldPos += 8;
          }
          if (compSize === 0xffffffff && fieldPos + 8 <= exPos + dataSize) {
            compSize = readU64LE(view, fieldPos);
            fieldPos += 8;
          }
          if (localOffset === 0xffffffff && fieldPos + 8 <= exPos + dataSize) {
            localOffset = readU64LE(view, fieldPos);
          }
          break;
        }
        exPos += dataSize;
      }
    }

    const entry = {
      name: normName,
      rawName,
      method,
      compSize,
      uncompSize,
      localOffset,
    };

    if (normName && !rawName.endsWith("/")) {
      entries.push(entry);
      exactMap.set(normName, entry);
      lowerMap.set(normName.toLowerCase(), entry);
      const decoded = normalizeZipPath(safeDecodeURI(normName));
      if (!lowerMap.has(decoded.toLowerCase())) {
        lowerMap.set(decoded.toLowerCase(), entry);
      }
    }

    cursor += 46 + nameLen + extraLen + commentLen;
  }

  function findEntry(targetPath) {
    const norm = normalizeZipPath(targetPath);
    if (exactMap.has(norm)) return exactMap.get(norm);
    const lower = norm.toLowerCase();
    if (lowerMap.has(lower)) return lowerMap.get(lower);
    const decoded = normalizeZipPath(safeDecodeURI(norm));
    if (exactMap.has(decoded)) return exactMap.get(decoded);
    if (lowerMap.has(decoded.toLowerCase())) return lowerMap.get(decoded.toLowerCase());
    return null;
  }

  async function getBytes(targetPath) {
    const entry = findEntry(targetPath);
    if (!entry) return null;
    const lh = entry.localOffset;
    if (lh + 30 > len || view.getUint32(lh, true) !== 0x04034b50) {
      throw new Error(`Invalid local ZIP header for ${entry.name}`);
    }
    const localNameLen = view.getUint16(lh + 26, true);
    const localExtraLen = view.getUint16(lh + 28, true);
    const dataStart = lh + 30 + localNameLen + localExtraLen;
    const dataEnd = dataStart + entry.compSize;
    if (dataEnd > len) {
      throw new Error(`Truncated ZIP entry data for ${entry.name}`);
    }
    const slice = bytes.subarray(dataStart, dataEnd);
    if (entry.method === 0) {
      return slice.slice();
    }
    if (entry.method === 8) {
      return inflateRaw(slice, entry.uncompSize);
    }
    throw new Error(`Unsupported ZIP compression method ${entry.method} in ${entry.name}`);
  }

  async function getText(targetPath) {
    const b = await getBytes(targetPath);
    if (!b) return null;
    return utf8Decoder.decode(b);
  }

  return {
    entries,
    has: (p) => findEntry(p) !== null,
    findEntry,
    getBytes,
    getText,
  };
}

/* ============================================================================
   3. Minimal Valid EPUB 3 Builder (Stored ZIP with real CRC-32)
   ============================================================================ */

function buildStoredZip(files) {
  const localParts = [];
  const cdParts = [];
  let offset = 0;

  for (const file of files) {
    const nameBytes = utf8Encoder.encode(file.name);
    const dataBytes =
      file.data instanceof Uint8Array
        ? file.data
        : utf8Encoder.encode(String(file.data));
    const crc = crc32(dataBytes);
    const size = dataBytes.byteLength;

    // Local File Header (30 bytes)
    const lh = new Uint8Array(30 + nameBytes.length);
    const lv = new DataView(lh.buffer);
    lv.setUint32(0, 0x04034b50, true);
    lv.setUint16(4, 20, true); // version needed
    lv.setUint16(6, 0x0800, true); // UTF-8 flag
    lv.setUint16(8, 0, true); // stored
    lv.setUint16(10, 0, true);
    lv.setUint16(12, 0, true);
    lv.setUint32(14, crc, true);
    lv.setUint32(18, size, true);
    lv.setUint32(22, size, true);
    lv.setUint16(26, nameBytes.length, true);
    lv.setUint16(28, 0, true);
    lh.set(nameBytes, 30);

    localParts.push(lh, dataBytes);

    // Central Directory Header (46 bytes)
    const cd = new Uint8Array(46 + nameBytes.length);
    const cv = new DataView(cd.buffer);
    cv.setUint32(0, 0x02014b50, true);
    cv.setUint16(4, 20, true);
    cv.setUint16(6, 20, true);
    cv.setUint16(8, 0x0800, true);
    cv.setUint16(10, 0, true);
    cv.setUint16(12, 0, true);
    cv.setUint16(14, 0, true);
    cv.setUint32(16, crc, true);
    cv.setUint32(20, size, true);
    cv.setUint32(24, size, true);
    cv.setUint16(28, nameBytes.length, true);
    cv.setUint16(30, 0, true);
    cv.setUint16(32, 0, true);
    cv.setUint16(34, 0, true);
    cv.setUint16(36, 0, true);
    cv.setUint32(38, 0, true);
    cv.setUint32(42, offset, true);
    cd.set(nameBytes, 46);

    cdParts.push(cd);
    offset += lh.byteLength + size;
  }

  let cdSize = 0;
  for (const p of cdParts) cdSize += p.byteLength;

  const eocd = new Uint8Array(22);
  const ev = new DataView(eocd.buffer);
  ev.setUint32(0, 0x06054b50, true);
  ev.setUint16(4, 0, true);
  ev.setUint16(6, 0, true);
  ev.setUint16(8, files.length, true);
  ev.setUint16(10, files.length, true);
  ev.setUint32(12, cdSize, true);
  ev.setUint32(16, offset, true);
  ev.setUint16(20, 0, true);

  const totalSize = offset + cdSize + 22;
  const result = new Uint8Array(totalSize);
  let pos = 0;
  for (const p of localParts) {
    result.set(p, pos);
    pos += p.byteLength;
  }
  for (const p of cdParts) {
    result.set(p, pos);
    pos += p.byteLength;
  }
  result.set(eocd, pos);
  return result;
}

function escapeXml(str) {
  return String(str || "")
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

/**
 * Builds a standards-compliant EPUB 3 Blob that runs through the exact parseEpub() pipeline.
 * @param {object} spec
 * @param {string} spec.id
 * @param {string} spec.title
 * @param {string} spec.author
 * @param {string} [spec.language="en"]
 * @param {string} [spec.description=""]
 * @param {string} [spec.date=""]
 * @param {string} [spec.publisher="Clothbound Public Domain Editions"]
 * @param {string} [spec.coverSvg] - SVG string for cover-image
 * @param {Array<{title: string, subtitle?: string, html: string}>} spec.chapters
 */
export function buildMinimalEpubBlob(spec) {
  const id = spec.id || "urn:uuid:clothbound-book";
  const lang = spec.language || "en";
  const chapters = spec.chapters || [];

  const files = [
    { name: "mimetype", data: "application/epub+zip" },
    {
      name: "META-INF/container.xml",
      data: `<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>`,
    },
  ];

  const manifestItems = [
    `<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>`,
  ];
  const spineItems = [];
  const navItems = [];

  if (spec.coverSvg) {
    manifestItems.push(
      `<item id="cover-image" href="cover.svg" media-type="image/svg+xml" properties="cover-image"/>`
    );
    files.push({ name: "OEBPS/cover.svg", data: spec.coverSvg });
  }

  chapters.forEach((ch, idx) => {
    const num = String(idx + 1).padStart(2, "0");
    const href = `ch${num}.xhtml`;
    const itemId = `ch${num}`;
    manifestItems.push(
      `<item id="${itemId}" href="${href}" media-type="application/xhtml+xml"/>`
    );
    spineItems.push(`<itemref idref="${itemId}"/>`);
    navItems.push(
      `<li><a href="${href}">${escapeXml(ch.title || `Chapter ${idx + 1}`)}</a></li>`
    );

    const kickerHtml = ch.kicker
      ? `<p class="cb-kicker">${escapeXml(ch.kicker)}</p>`
      : "";
    const headingHtml = ch.title
      ? `<h1>${escapeXml(ch.title)}</h1>`
      : "";
    const subHtml = ch.subtitle
      ? `<p class="cb-subtitle">${escapeXml(ch.subtitle)}</p>`
      : "";

    const xhtml = `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="${escapeXml(lang)}" lang="${escapeXml(lang)}">
<head>
  <meta charset="utf-8"/>
  <title>${escapeXml(ch.title || spec.title)}</title>
</head>
<body>
  <section class="cb-chapter">
    ${kickerHtml}
    ${headingHtml}
    ${subHtml}
    ${ch.html}
  </section>
</body>
</html>`;
    files.push({ name: `OEBPS/${href}`, data: xhtml });
  });

  const navXhtml = `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="${escapeXml(lang)}">
<head><meta charset="utf-8"/><title>Contents</title></head>
<body>
  <nav epub:type="toc" id="toc">
    <h1>Contents</h1>
    <ol>${navItems.join("\n      ")}</ol>
  </nav>
</body>
</html>`;
  files.push({ name: "OEBPS/nav.xhtml", data: navXhtml });

  const opf = `<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="pub-id">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="pub-id">${escapeXml(id)}</dc:identifier>
    <dc:title>${escapeXml(spec.title || "Untitled")}</dc:title>
    <dc:creator>${escapeXml(spec.author || "Unknown Author")}</dc:creator>
    <dc:language>${escapeXml(lang)}</dc:language>
    ${spec.description ? `<dc:description>${escapeXml(spec.description)}</dc:description>` : ""}
    ${spec.publisher ? `<dc:publisher>${escapeXml(spec.publisher)}</dc:publisher>` : ""}
    ${spec.date ? `<dc:date>${escapeXml(spec.date)}</dc:date>` : ""}
  </metadata>
  <manifest>
    ${manifestItems.join("\n    ")}
  </manifest>
  <spine>
    ${spineItems.join("\n    ")}
  </spine>
</package>`;
  files.push({ name: "OEBPS/content.opf", data: opf });

  const zipBytes = buildStoredZip(files);
  return new Blob([zipBytes], { type: "application/epub+zip" });
}

/* ============================================================================
   4. XML / Path / Media Helpers & Sanitizer
   ============================================================================ */

function dirname(filePath) {
  const idx = filePath.lastIndexOf("/");
  return idx === -1 ? "" : filePath.slice(0, idx);
}

function resolveRelativePath(baseDir, relHref) {
  if (!relHref) return "";
  const clean = relHref.split("#")[0].split("?")[0].trim();
  if (!clean) return "";
  if (/^[a-z][a-z0-9+.-]*:/i.test(clean)) return "";
  const combined = baseDir ? `${baseDir}/${clean}` : clean;
  return normalizeZipPath(safeDecodeURI(combined));
}

function splitHref(href) {
  const s = String(href || "").trim();
  const hashIdx = s.indexOf("#");
  if (hashIdx === -1) return { path: s, fragment: "" };
  return { path: s.slice(0, hashIdx), fragment: s.slice(hashIdx + 1) };
}

function guessMimeType(pathStr, declared) {
  if (declared && declared !== "application/octet-stream") return declared;
  const lower = String(pathStr || "").toLowerCase();
  if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) return "image/jpeg";
  if (lower.endsWith(".png")) return "image/png";
  if (lower.endsWith(".gif")) return "image/gif";
  if (lower.endsWith(".webp")) return "image/webp";
  if (lower.endsWith(".svg")) return "image/svg+xml";
  if (lower.endsWith(".css")) return "text/css";
  if (lower.endsWith(".woff2")) return "font/woff2";
  if (lower.endsWith(".woff")) return "font/woff";
  if (lower.endsWith(".ttf")) return "font/ttf";
  if (lower.endsWith(".otf")) return "font/otf";
  return declared || "application/octet-stream";
}

function bytesToDataUrl(bytes, mimeType) {
  let binary = "";
  const chunkSize = 0x8000;
  for (let i = 0; i < bytes.length; i += chunkSize) {
    const sub = bytes.subarray(i, i + chunkSize);
    binary += String.fromCharCode.apply(null, sub);
  }
  return `data:${mimeType};base64,${btoa(binary)}`;
}

function parseXmlDoc(xmlText) {
  const parser = new DOMParser();
  const doc = parser.parseFromString(xmlText, "application/xml");
  if (doc.querySelector("parsererror")) {
    return parser.parseFromString(xmlText, "text/html");
  }
  return doc;
}

function getElementsByLocalName(root, localName) {
  const target = localName.toLowerCase();
  const out = [];
  const all = root.getElementsByTagName("*");
  for (let i = 0; i < all.length; i++) {
    const el = all[i];
    const ln = (el.localName || el.tagName || "").toLowerCase();
    if (ln === target || ln.endsWith(`:${target}`)) {
      out.push(el);
    }
  }
  return out;
}

function isDangerousUrl(val) {
  const trimmed = String(val || "").replace(/[\u0000-\u0020]+/g, "").toLowerCase();
  return (
    trimmed.startsWith("javascript:") ||
    trimmed.startsWith("vbscript:") ||
    trimmed.startsWith("data:text/html")
  );
}

function isExternalUrl(val) {
  return /^(?:https?:)?\/\//i.test(String(val || "").trim());
}

/**
 * Sanitizes publisher CSS for local-only scoped rendering while keeping
 * Clothbound's reading themes and pagination in control.
 */
async function sanitizeAndScopeCss(rawCss, cssDir, getBlobUrlForPath) {
  if (!rawCss) return "";
  // Strip @import (especially remote) and @font-face remote fetches
  let css = rawCss.replace(/@import\s+[^;]+;/gi, "");

  // Rewrite url(...) references: strip external http(s), map local assets to Blob URLs
  const matches = [...css.matchAll(/url\(\s*(['"]?)([^)'"]+)\1\s*\)/gi)];
  for (const m of matches) {
    const full = m[0];
    const target = m[2].trim();
    if (!target || target.startsWith("data:")) continue;
    if (isExternalUrl(target) || isDangerousUrl(target)) {
      css = css.replace(full, "none");
      continue;
    }
    const resolved = resolveRelativePath(cssDir, target);
    const blobUrl = resolved ? await getBlobUrlForPath(resolved) : null;
    css = css.replace(full, blobUrl ? `url("${blobUrl}")` : "none");
  }

  // Strip global overrides on html/body colors/backgrounds/fixed heights that break pagination/themes
  css = css.replace(/@page\s*\{[^}]*\}/gi, "");
  css = css.replace(/position\s*:\s*fixed\b/gi, "position: relative");

  // Scope simple selectors inside .cb-epub-content using @scope fallback or safe prefixing
  const scoped = [];
  const ruleRe = /([^{}@]+)\{([^{}]*)\}/g;
  let match;
  while ((match = ruleRe.exec(css)) !== null) {
    const rawSel = match[1].trim();
    let decls = match[2].trim();
    if (!rawSel || !decls) continue;

    // Remove hard-coded ink/paper colors & root font overrides so Paper/Sepia/Dusk/Charcoal themes always look right
    decls = decls
      .replace(/(?:^|;)\s*(?:background(?:-color)?|color|font-family|font-size|line-height|width|height|max-width|margin-left|margin-right)\s*:[^;]+/gi, "")
      .trim();
    if (!decls) continue;

    const selectors = rawSel
      .split(",")
      .map((s) => s.trim())
      .filter((s) => s && !/^(?:html|body|\*)$/i.test(s))
      .map((s) => `.cb-epub-content ${s.replace(/^(?:html|body)\s+/i, "")}`);

    if (selectors.length > 0) {
      scoped.push(`${selectors.join(", ")} { ${decls} }`);
    }
  }
  return scoped.join("\n");
}

/* ============================================================================
   5. EPUB Book Parser & Session Object
   ============================================================================ */

export async function parseEpub(blobOrBuffer) {
  const buffer =
    blobOrBuffer instanceof ArrayBuffer
      ? blobOrBuffer
      : blobOrBuffer instanceof Uint8Array
        ? blobOrBuffer.buffer.slice(
            blobOrBuffer.byteOffset,
            blobOrBuffer.byteOffset + blobOrBuffer.byteLength
          )
        : await blobOrBuffer.arrayBuffer();

  const zip = readZip(buffer);

  // 1. Locate OPF via META-INF/container.xml
  let opfPath = "";
  const containerXml = await zip.getText("META-INF/container.xml");
  if (containerXml) {
    const containerDoc = parseXmlDoc(containerXml);
    const rootfiles = getElementsByLocalName(containerDoc, "rootfile");
    for (const rf of rootfiles) {
      const fp = rf.getAttribute("full-path");
      if (fp) {
        opfPath = normalizeZipPath(fp);
        break;
      }
    }
  }
  if (!opfPath) {
    const opfEntry = zip.entries.find((e) => e.name.toLowerCase().endsWith(".opf"));
    if (!opfEntry) {
      throw new Error("EPUB is missing META-INF/container.xml and OPF package file.");
    }
    opfPath = opfEntry.name;
  }

  const opfXml = await zip.getText(opfPath);
  if (!opfXml) {
    throw new Error(`Could not read OPF package at ${opfPath}`);
  }
  const opfDoc = parseXmlDoc(opfXml);
  const opfDir = dirname(opfPath);

  // 2. Metadata
  const firstText = (name) => {
    const els = getElementsByLocalName(opfDoc, name);
    for (const el of els) {
      const txt = (el.textContent || "").trim();
      if (txt) return txt;
    }
    return "";
  };

  const metadata = {
    title: firstText("title") || "Untitled Book",
    author: firstText("creator") || "Unknown Author",
    language: firstText("language") || "en",
    description: firstText("description") || "",
    publisher: firstText("publisher") || "",
    date: firstText("date") || "",
    identifier: firstText("identifier") || "",
  };

  // 3. Manifest
  const manifestById = new Map();
  const manifestByPath = new Map();
  let navItem = null;
  let ncxItem = null;
  let coverItem = null;

  const itemEls = getElementsByLocalName(opfDoc, "item");
  for (const el of itemEls) {
    const id = el.getAttribute("id") || "";
    const href = el.getAttribute("href") || "";
    const mediaType = (el.getAttribute("media-type") || "").toLowerCase();
    const properties = (el.getAttribute("properties") || "").split(/\s+/).filter(Boolean);
    const fullPath = resolveRelativePath(opfDir, href);

    const item = { id, href, fullPath, mediaType, properties };
    if (id) manifestById.set(id, item);
    if (fullPath) manifestByPath.set(fullPath.toLowerCase(), item);

    if (properties.includes("nav")) navItem = item;
    if (mediaType === "application/x-dtbncx+xml") ncxItem = item;
    if (properties.includes("cover-image")) coverItem = item;
  }

  // Fallback cover via <meta name="cover" content="id-or-href">
  if (!coverItem) {
    const metaEls = getElementsByLocalName(opfDoc, "meta");
    for (const m of metaEls) {
      if ((m.getAttribute("name") || "").toLowerCase() === "cover") {
        const ref = (m.getAttribute("content") || "").trim();
        if (ref && manifestById.has(ref)) {
          coverItem = manifestById.get(ref);
          break;
        }
        if (ref) {
          const resolved = resolveRelativePath(opfDir, ref).toLowerCase();
          if (manifestByPath.has(resolved)) {
            coverItem = manifestByPath.get(resolved);
            break;
          }
        }
      }
    }
  }
  if (!coverItem) {
    for (const item of manifestById.values()) {
      if (
        item.mediaType.startsWith("image/") &&
        /cover/i.test(`${item.id} ${item.href}`)
      ) {
        coverItem = item;
        break;
      }
    }
  }

  let coverDataUrl = "";
  if (coverItem && coverItem.fullPath) {
    try {
      const coverBytes = await zip.getBytes(coverItem.fullPath);
      if (coverBytes && coverBytes.byteLength > 0 && coverBytes.byteLength < 6 * 1024 * 1024) {
        const mime = guessMimeType(coverItem.fullPath, coverItem.mediaType);
        coverDataUrl = bytesToDataUrl(coverBytes, mime);
      }
    } catch {
      coverDataUrl = "";
    }
  }

  // 4. Spine
  const spineEls = getElementsByLocalName(opfDoc, "spine");
  const spineTocAttr = spineEls[0]?.getAttribute("toc") || "";
  if (!ncxItem && spineTocAttr && manifestById.has(spineTocAttr)) {
    ncxItem = manifestById.get(spineTocAttr);
  }

  const itemRefEls = getElementsByLocalName(opfDoc, "itemref");
  const linearSpine = [];
  const allSpine = [];
  for (const ref of itemRefEls) {
    const idref = ref.getAttribute("idref") || "";
    const item = manifestById.get(idref);
    if (!item || !item.fullPath) continue;
    allSpine.push(item);
    if ((ref.getAttribute("linear") || "yes").toLowerCase() !== "no") {
      linearSpine.push(item);
    }
  }
  const spine = linearSpine.length > 0 ? linearSpine : allSpine;
  if (spine.length === 0) {
    throw new Error("EPUB spine contains no readable chapters.");
  }

  const spinePathToIndex = new Map();
  spine.forEach((item, idx) => {
    spinePathToIndex.set(item.fullPath.toLowerCase(), idx);
  });

  // 5. Table of Contents (EPUB 3 nav.xhtml -> EPUB 2 toc.ncx -> spine fallback)
  const toc = [];

  if (navItem && navItem.fullPath) {
    const navText = await zip.getText(navItem.fullPath);
    if (navText) {
      const navDoc = parseXmlDoc(navText);
      const navDir = dirname(navItem.fullPath);
      const navEls = getElementsByLocalName(navDoc, "nav");
      let tocNav =
        navEls.find((n) => {
          const epubType =
            n.getAttribute("epub:type") ||
            n.getAttributeNS("http://www.idpf.org/2007/ops", "type") ||
            "";
          return /\btoc\b/i.test(epubType);
        }) || navEls[0];

      const walkList = (container, depth) => {
        if (!container) return;
        for (const child of Array.from(container.children)) {
          const tag = (child.localName || child.tagName || "").toLowerCase();
          if (tag === "ol" || tag === "ul") {
            walkList(child, depth);
          } else if (tag === "li") {
            let link = null;
            for (const liChild of Array.from(child.children)) {
              const ct = (liChild.localName || liChild.tagName || "").toLowerCase();
              if (ct === "a" && !link) link = liChild;
            }
            if (link) {
              const label = (link.textContent || "").replace(/\s+/g, " ").trim();
              const rawHref = link.getAttribute("href") || "";
              const { path: relPath, fragment } = splitHref(rawHref);
              const fullPath = resolveRelativePath(navDir, relPath);
              const chapterIndex = spinePathToIndex.get(fullPath.toLowerCase());
              if (label && chapterIndex !== undefined) {
                toc.push({ title: label, chapterIndex, fragment, depth });
              }
            }
            for (const liChild of Array.from(child.children)) {
              const ct = (liChild.localName || liChild.tagName || "").toLowerCase();
              if (ct === "ol" || ct === "ul") walkList(liChild, depth + 1);
            }
          }
        }
      };

      if (tocNav) walkList(tocNav, 0);
    }
  }

  if (toc.length === 0 && ncxItem && ncxItem.fullPath) {
    const ncxText = await zip.getText(ncxItem.fullPath);
    if (ncxText) {
      const ncxDoc = parseXmlDoc(ncxText);
      const ncxDir = dirname(ncxItem.fullPath);
      const navMap = getElementsByLocalName(ncxDoc, "navMap")[0];

      const walkNavPoints = (parent, depth) => {
        if (!parent) return;
        for (const child of Array.from(parent.children)) {
          const tag = (child.localName || child.tagName || "").toLowerCase();
          if (tag !== "navpoint") continue;
          const textEl = getElementsByLocalName(child, "text")[0];
          const contentEl = getElementsByLocalName(child, "content")[0];
          const label = (textEl?.textContent || "").replace(/\s+/g, " ").trim();
          const rawSrc = contentEl?.getAttribute("src") || "";
          const { path: relPath, fragment } = splitHref(rawSrc);
          const fullPath = resolveRelativePath(ncxDir, relPath);
          const chapterIndex = spinePathToIndex.get(fullPath.toLowerCase());
          if (label && chapterIndex !== undefined) {
            toc.push({ title: label, chapterIndex, fragment, depth });
          }
          walkNavPoints(child, depth + 1);
        }
      };
      walkNavPoints(navMap, 0);
    }
  }

  if (toc.length === 0) {
    spine.forEach((item, idx) => {
      const fallbackTitle =
        item.href
          .replace(/^.*\//, "")
          .replace(/\.[^.]+$/, "")
          .replace(/[-_]+/g, " ") || `Section ${idx + 1}`;
      toc.push({
        title: fallbackTitle.charAt(0).toUpperCase() + fallbackTitle.slice(1),
        chapterIndex: idx,
        fragment: "",
        depth: 0,
      });
    });
  }

  // Track created Blob URLs so dispose() cleans up all memory
  const objectUrls = new Map();
  const chapterCache = new Map();
  let disposed = false;

  async function getBlobUrlForPath(zipPath) {
    if (disposed || !zipPath) return null;
    const key = zipPath.toLowerCase();
    if (objectUrls.has(key)) return objectUrls.get(key);
    const bytes = await zip.getBytes(zipPath);
    if (!bytes) return null;
    const manifestEntry = manifestByPath.get(key);
    const mime = guessMimeType(zipPath, manifestEntry?.mediaType);
    const url = URL.createObjectURL(new Blob([bytes], { type: mime }));
    objectUrls.set(key, url);
    return url;
  }

  async function loadChapter(chapterIndex) {
    if (disposed) throw new Error("Book has been closed.");
    if (chapterIndex < 0 || chapterIndex >= spine.length) {
      throw new Error(`Chapter index out of bounds: ${chapterIndex}`);
    }
    if (chapterCache.has(chapterIndex)) return chapterCache.get(chapterIndex);

    const spineItem = spine[chapterIndex];
    const rawHtml = await zip.getText(spineItem.fullPath);
    if (rawHtml === null) {
      throw new Error(`Missing chapter file: ${spineItem.fullPath}`);
    }

    const chapterDir = dirname(spineItem.fullPath);
    const parser = new DOMParser();
    let doc = parser.parseFromString(rawHtml, "application/xhtml+xml");
    if (doc.querySelector("parsererror") || !doc.body) {
      doc = parser.parseFromString(rawHtml, "text/html");
    }

    // Collect local stylesheets before removing <head>
    const cssParts = [];
    const linkEls = Array.from(doc.querySelectorAll("link"));
    for (const link of linkEls) {
      const rel = (link.getAttribute("rel") || "").toLowerCase();
      const href = link.getAttribute("href") || "";
      if (rel.includes("stylesheet") && href && !isExternalUrl(href)) {
        const cssPath = resolveRelativePath(chapterDir, href);
        const cssText = cssPath ? await zip.getText(cssPath) : null;
        if (cssText) {
          cssParts.push(
            await sanitizeAndScopeCss(cssText, dirname(cssPath), getBlobUrlForPath)
          );
        }
      }
    }
    const styleEls = Array.from(doc.querySelectorAll("style"));
    for (const st of styleEls) {
      if (st.textContent) {
        cssParts.push(
          await sanitizeAndScopeCss(st.textContent, chapterDir, getBlobUrlForPath)
        );
      }
    }

    // Clone body into a clean HTML document container for sanitization
    const htmlDoc = document.implementation.createHTMLDocument("");
    const container = htmlDoc.createElement("div");
    if (doc.body) {
      for (const child of Array.from(doc.body.childNodes)) {
        container.appendChild(htmlDoc.importNode(child, true));
      }
    }

    // Remove forbidden active / external / unsafe tags
    const forbiddenTags = [
      "script",
      "iframe",
      "frame",
      "frameset",
      "object",
      "embed",
      "applet",
      "form",
      "base",
      "style",
      "link",
      "meta",
      "noscript",
    ];
    for (const tag of forbiddenTags) {
      for (const el of Array.from(container.querySelectorAll(tag))) {
        el.remove();
      }
    }

    // Walk all elements to sanitize attributes & rewrite local media/links
    const allEls = Array.from(container.querySelectorAll("*"));
    for (const el of allEls) {
      const tag = el.tagName.toLowerCase();

      // Strip event handlers & unsafe URLs
      for (const attr of Array.from(el.attributes)) {
        const name = attr.name.toLowerCase();
        const val = attr.value || "";
        if (name.startsWith("on") || name === "srcdoc") {
          el.removeAttribute(attr.name);
          continue;
        }
        if (
          (name === "href" ||
            name === "src" ||
            name === "xlink:href" ||
            name === "poster" ||
            name === "action") &&
          isDangerousUrl(val)
        ) {
          el.removeAttribute(attr.name);
        }
      }

      // Rewrite <img src>
      if (tag === "img") {
        const src = el.getAttribute("src") || "";
        if (!src || isExternalUrl(src)) {
          el.removeAttribute("src");
          el.setAttribute("data-blocked-external", "true");
        } else if (!src.startsWith("data:")) {
          const resolved = resolveRelativePath(chapterDir, src);
          const blobUrl = resolved ? await getBlobUrlForPath(resolved) : null;
          if (blobUrl) {
            el.setAttribute("src", blobUrl);
          } else {
            el.removeAttribute("src");
          }
        }
        el.removeAttribute("srcset");
      }

      // Rewrite SVG <image href / xlink:href>
      if (tag === "image") {
        const href =
          el.getAttribute("href") ||
          el.getAttribute("xlink:href") ||
          el.getAttributeNS("http://www.w3.org/1999/xlink", "href") ||
          "";
        if (!href || isExternalUrl(href)) {
          el.removeAttribute("href");
          el.removeAttribute("xlink:href");
        } else if (!href.startsWith("data:")) {
          const resolved = resolveRelativePath(chapterDir, href);
          const blobUrl = resolved ? await getBlobUrlForPath(resolved) : null;
          if (blobUrl) {
            el.setAttribute("href", blobUrl);
            el.removeAttribute("xlink:href");
          }
        }
      }

      // Strip external media sources
      if (tag === "audio" || tag === "video" || tag === "source") {
        const src = el.getAttribute("src") || "";
        if (isExternalUrl(src)) el.removeAttribute("src");
      }

      // Rewrite <a> links for safe internal navigation
      if (tag === "a") {
        const href = el.getAttribute("href") || "";
        if (!href) continue;
        if (isExternalUrl(href) || href.startsWith("mailto:")) {
          el.setAttribute("target", "_blank");
          el.setAttribute("rel", "noopener noreferrer");
        } else {
          const { path: relPath, fragment } = splitHref(href);
          const targetPath = relPath
            ? resolveRelativePath(chapterDir, relPath)
            : spineItem.fullPath;
          const targetIdx = spinePathToIndex.get(targetPath.toLowerCase());
          if (targetIdx !== undefined) {
            el.setAttribute("data-epub-chapter", String(targetIdx));
            if (fragment) el.setAttribute("data-epub-fragment", fragment);
            el.setAttribute("href", `#cb-ch-${targetIdx}${fragment ? `-${fragment}` : ""}`);
          } else {
            el.removeAttribute("href");
          }
        }
      }
    }

    // Mark first substantive paragraph for typographic drop cap
    const firstPara = Array.from(container.querySelectorAll("p")).find((p) => {
      if (
        p.classList.contains("cb-kicker") ||
        p.classList.contains("cb-subtitle") ||
        p.closest("blockquote") ||
        p.closest("figcaption")
      ) {
        return false;
      }
      const txt = (p.textContent || "").trim();
      return txt.length >= 40 && /^[A-Za-z“"‘']/.test(txt);
    });
    if (firstPara) {
      firstPara.classList.add("cb-opener");
    }

    const headingEl = container.querySelector("h1, h2, h3");
    const tocMatch = toc.find((t) => t.chapterIndex === chapterIndex);
    const title =
      tocMatch?.title ||
      (headingEl?.textContent || "").replace(/\s+/g, " ").trim() ||
      `Section ${chapterIndex + 1}`;

    const plainText = (container.textContent || "").replace(/\s+/g, " ").trim();
    const wordCount = plainText ? plainText.split(/\s+/).length : 0;

    const result = {
      chapterIndex,
      title,
      html: container.innerHTML,
      css: cssParts.filter(Boolean).join("\n"),
      plainText,
      wordCount,
    };
    chapterCache.set(chapterIndex, result);
    return result;
  }

  async function searchBook(query, maxResults = 60) {
    const q = String(query || "").trim().toLowerCase();
    if (q.length < 2) return [];
    const results = [];
    for (let i = 0; i < spine.length; i++) {
      const ch = await loadChapter(i);
      const text = ch.plainText;
      const lower = text.toLowerCase();
      let from = 0;
      while (results.length < maxResults) {
        const idx = lower.indexOf(q, from);
        if (idx === -1) break;
        const start = Math.max(0, idx - 54);
        const end = Math.min(text.length, idx + q.length + 74);
        const snippet =
          (start > 0 ? "…" : "") +
          text.slice(start, end).trim() +
          (end < text.length ? "…" : "");
        results.push({
          chapterIndex: i,
          chapterTitle: ch.title,
          offset: idx,
          ratio: text.length > 0 ? idx / text.length : 0,
          snippet,
          match: text.slice(idx, idx + q.length),
        });
        from = idx + q.length;
      }
      if (results.length >= maxResults) break;
    }
    return results;
  }

  function dispose() {
    if (disposed) return;
    disposed = true;
    for (const url of objectUrls.values()) {
      try {
        URL.revokeObjectURL(url);
      } catch {}
    }
    objectUrls.clear();
    chapterCache.clear();
  }

  return {
    metadata,
    coverDataUrl,
    spine,
    chapterCount: spine.length,
    toc,
    loadChapter,
    searchBook,
    dispose,
  };
}
