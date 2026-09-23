# -*- coding: utf-8 -*-
"""Python 移植 QuickLZ 1.5 level3 解压核心，验证协议例子数据是否 QuickLZ 压缩"""
import sys, io, re
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

def le32(b, off):
    return b[off] | (b[off+1] << 8) | (b[off+2] << 16) | (b[off+3] << 24)

def fast_read(src, off, bytes_):
    if bytes_ == 4: return le32(src, off)
    if bytes_ == 3: return src[off] | (src[off+1] << 8) | (src[off+2] << 16)
    if bytes_ == 2: return src[off] | (src[off+1] << 8)
    return src[off]

def qlz_size_header(src):
    n = 4 if (src[0] & 2) == 2 else 1
    return 2 * n + 1

def qlz_size_compressed(src):
    n = 4 if (src[0] & 2) == 2 else 1
    return fast_read(src, 1, n)

def qlz_size_decompressed(src):
    n = 4 if (src[0] & 2) == 2 else 1
    return fast_read(src, 1 + n, n)

BITLUT = [4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0]
UNCONDITIONAL_MATCHLEN = 6
UNCOMPRESSED_END = 4
CWORD_LEN = 4

def qlz_decompress_core(source, size):
    """level 3 解压核心。source=压缩块, size=解压后大小"""
    src = qlz_size_header(source)
    dst = bytearray()
    last_destination_byte = size - 1
    cword_val = 1
    last_matchstart = last_destination_byte - UNCONDITIONAL_MATCHLEN - UNCOMPRESSED_END
    last_source_byte = qlz_size_compressed(source) - 1
    n_data = len(source)

    while True:
        if cword_val == 1:
            if src + CWORD_LEN - 1 > last_source_byte:
                break
            cword_val = fast_read(source, src, CWORD_LEN)
            src += CWORD_LEN

        if src + 4 - 1 > last_source_byte:
            break

        fetch = fast_read(source, src, 4)

        if (cword_val & 1) == 1:
            # ---- 匹配分支 (level 3) ----
            cword_val = cword_val >> 1
            if (fetch & 3) == 0:
                offset = (fetch & 0xff) >> 2
                matchlen = 3
                src += 1
            elif (fetch & 2) == 0:
                offset = (fetch & 0xffff) >> 2
                matchlen = 3
                src += 2
            elif (fetch & 1) == 0:
                offset = (fetch & 0xffff) >> 6
                matchlen = ((fetch >> 2) & 15) + 3
                src += 2
            elif (fetch & 127) != 3:
                offset = (fetch >> 7) & 0x1ffff
                matchlen = ((fetch >> 2) & 0x1f) + 2
                src += 3
            else:
                offset = fetch >> 15
                matchlen = ((fetch >> 7) & 255) + 3
                src += 4
            offset2 = len(dst) - offset
            if offset2 < 0:
                return None
            # memcpy_up: 逐字节拷贝（允许重叠）
            for i in range(matchlen):
                dst.append(dst[offset2 + i])
        else:
            # ---- 字面量分支 ----
            if len(dst) < last_matchstart:
                n = BITLUT[cword_val & 0xf]
                if n == 0:
                    n = 1
                # X86X64 拷贝 4 字节但只前进 n 字节（Python 只需拷贝 n 字节）
                cnt = min(n, n_data - src)
                dst += source[src:src + cnt]
                cword_val = cword_val >> n
                dst = dst  # no-op
                src += n
            else:
                while len(dst) <= last_destination_byte:
                    if cword_val == 1:
                        src += CWORD_LEN
                        cword_val = 1 << 31
                    if src >= n_data:
                        break
                    dst.append(source[src])
                    src += 1
                    cword_val = cword_val >> 1
                break

        if len(dst) >= size:
            break

    return bytes(dst[:size])

# ============ 验证协议例子数据 ============
txt = open(r'../src/main.c', encoding='utf-8').read()
m = re.search(r'static const uint8_t frame84_data\[\] = \{(.*?)\};', txt, re.S)
vals = re.findall(r'0x([0-9A-Fa-f]{2})', m.group(1))
data = bytes(int(v, 16) for v in vals)
img = data[3:3 + 233]
print(f'协议例子数据: {len(img)}B')
print(f'开头: {img[:20].hex(" ").upper()}')

# packed 格式: 4B 大端块头 + QuickLZ 块
be32 = lambda b: (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]
off = 0
while off + 4 <= len(img):
    blk_size = be32(img[off:off + 4])
    if blk_size == 0 or off + 4 + blk_size > len(img):
        break
    blk = img[off + 4:off + 4 + blk_size]
    decomp_size = qlz_size_decompressed(blk)
    print(f'\n[块] 块头={blk_size}B flags=0x{blk[0]:02X} 压缩={qlz_size_compressed(blk)}B 解压={decomp_size}B')
    out = qlz_decompress_core(blk, decomp_size)
    if out is None:
        print('  ❌ 解压失败（返回 None）')
    else:
        print(f'  ✅ 解压成功: {len(out)}B')
        print(f'  内容开头: {out[:32].hex(" ").upper()}')
        # 统计
        zeros = out.count(0)
        ff = out.count(0xFF)
        print(f'  0x00 数量: {zeros}  0xFF 数量: {ff}  其他: {len(out) - zeros - ff}')
        # 显示为图案（1bit 视图，37B/行 或 74B/行）
        if decomp_size == 2048:
            print('  (块内 2048B 是 QLZ_DEFAULT_BLOCK_SIZE 的原始点阵片段)')
    off += 4 + blk_size

print()
print('=== 结论 ===')
print('如果上面解压成功且内容合理 → 协议例子数据 = QuickLZ packed 压缩，实锤！')
