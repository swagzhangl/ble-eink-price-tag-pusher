// 验证黑层/红层解压后的实际内容分布（复用 decompress_render.js 的解压核心）
const fs = require('fs');

function le32(b, off) { return ((b[off] | (b[off+1] << 8) | (b[off+2] << 16) | (b[off+3] << 24)) >>> 0); }
function fastRead(src, off, n) {
  if (n === 4) return le32(src, off);
  if (n === 3) return (src[off] | (src[off+1] << 8) | (src[off+2] << 16)) >>> 0;
  if (n === 2) return (src[off] | (src[off+1] << 8)) >>> 0;
  return src[off];
}
function qlzSizeHeader(src) { const n = (src[0] & 2) === 2 ? 4 : 1; return 2*n + 1; }
function qlzSizeCompressed(src) { const n = (src[0] & 2) === 2 ? 4 : 1; return fastRead(src, 1, n); }
function qlzSizeDecompressed(src) { const n = (src[0] & 2) === 2 ? 4 : 1; return fastRead(src, 1+n, n); }

const BITLUT = [4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0];
const UNCONDITIONAL_MATCHLEN = 6, UNCOMPRESSED_END = 4, CWORD_LEN = 4;

function qlzDecompressCore(source, size) {
  let src = qlzSizeHeader(source);
  const dst = [];
  const lastDestinationByte = size - 1;
  let cwordVal = 1;
  const lastMatchstart = lastDestinationByte - UNCONDITIONAL_MATCHLEN - UNCOMPRESSED_END;
  const lastSourceByte = qlzSizeCompressed(source) - 1;
  const nData = source.length;

  while (true) {
    if (cwordVal === 1) {
      if (src + CWORD_LEN - 1 > lastSourceByte) break;
      cwordVal = fastRead(source, src, CWORD_LEN);
      src += CWORD_LEN;
    }
    if (src + 4 - 1 > lastSourceByte) break;
    const fetch = fastRead(source, src, 4);

    if ((cwordVal & 1) === 1) {
      cwordVal = cwordVal >>> 1;
      let offset, matchlen;
      if ((fetch & 3) === 0) { offset = (fetch & 0xff) >> 2; matchlen = 3; src += 1; }
      else if ((fetch & 2) === 0) { offset = (fetch & 0xffff) >> 2; matchlen = 3; src += 2; }
      else if ((fetch & 1) === 0) { offset = (fetch & 0xffff) >> 6; matchlen = ((fetch >> 2) & 15) + 3; src += 2; }
      else if ((fetch & 127) !== 3) { offset = (fetch >> 7) & 0x1ffff; matchlen = ((fetch >> 2) & 0x1f) + 2; src += 3; }
      else { offset = fetch >>> 15; matchlen = ((fetch >>> 7) & 255) + 3; src += 4; }
      const offset2 = dst.length - offset;
      if (offset2 < 0) return null;
      for (let i = 0; i < matchlen; i++) dst.push(dst[offset2 + i]);
    } else {
      if (dst.length < lastMatchstart) {
        let n = BITLUT[cwordVal & 0xf];
        if (n === 0) n = 1;
        for (let i = 0; i < n && src + i < nData; i++) dst.push(source[src + i]);
        cwordVal = cwordVal >>> n;
        src += n;
      } else {
        while (dst.length <= lastDestinationByte) {
          if (cwordVal === 1) { src += CWORD_LEN; cwordVal = 0x80000000; }
          if (src >= nData) break;
          dst.push(source[src]); src += 1;
          cwordVal = cwordVal >>> 1;
        }
        break;
      }
    }
    if (dst.length >= size) break;
  }
  return Buffer.from(dst.slice(0, size));
}

function decompressPacked(data) {
  let off = 0; const blocks = [];
  while (off + 4 <= data.length) {
    const blkSize = (data[off] << 24) | (data[off+1] << 16) | (data[off+2] << 8) | data[off+3];
    if (blkSize === 0 || off + 4 + blkSize > data.length) break;
    const blk = data.subarray(off+4, off+4+blkSize);
    const out = qlzDecompressCore(blk, qlzSizeDecompressed(blk));
    if (!out) return null;
    blocks.push(out);
    off += 4 + blkSize;
  }
  return Buffer.concat(blocks);
}

function loadCArray(p) {
  const t = fs.readFileSync(p, 'utf-8');
  const m = t.match(/static const uint8_t \w+\[(\d+)\] = \{(.*?)\};/s);
  const re = /0x([0-9A-Fa-f]{2})/g; const arr = []; let x;
  while ((x = re.exec(m[2])) !== null) arr.push(parseInt(x[1], 16));
  return Buffer.from(arr);
}

const img = decompressPacked(loadCArray('../src/compressed_image.h'));
const rev = decompressPacked(loadCArray('../src/compressed_rev.h'));
console.log('compressed_image 解压:', img ? img.length + 'B' : '失败');
console.log('compressed_rev 解压:', rev ? rev.length + 'B' : '失败');

function stat(name, buf) {
  let z = 0, f = 0;
  for (const b of buf) { if (b === 0) z++; else if (b === 0xFF) f++; }
  console.log(name + ': 0x00=' + z + ' (' + (z/buf.length*100).toFixed(1) + '%), 0xFF=' + f + ' (' + (f/buf.length*100).toFixed(1) + '%), 其他=' + (buf.length-z-f));
}
stat('黑层 type2 (compressed_image)', img);
stat('红层 type1 (compressed_rev)', rev);

const raw = loadCArray('../src/image_data.h');
console.log('黑层 与 image_data.h 完全一致:', img.equals(raw));

const revCheck = Buffer.from(img.map(b => ~b & 0xFF));
console.log('红层 与 [黑层逐字节取反] 完全一致:', rev.equals(revCheck));
