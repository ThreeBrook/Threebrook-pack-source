"""与 libtiff（tif_lzw.c）逐字节一致的 TIFF LZW 编码器，用于复现式还原。

imagecodecs 的 LZW 编码在码表填满时的处理与 libtiff 不同，噪声大的条带会在码表满后开始出现差异，
而现场 TIFF 大多由 libtiff 写出（OpenCV、多数视觉软件），所以单独实现 libtiff 的行为：
- 每个条带 / 图块开头输出 CLEAR；
- free_ent 超过当前最大码时码宽 +1（9 → 12 位）；
- free_ent 到 CODE_MAX-1（4094）时输出 CLEAR 并重置；
- 每 CHECK_GAP（10000）个输入字节检查一次压缩率，压缩率不升反降时输出 CLEAR 并重置；
- 结束时输出最后一个码与 EOI，按 MSB 优先补齐末字节。
字典查找用 Python dict 代替 libtiff 的散列表 —— 散列只影响查找速度，不影响输出的码。
纯整数运算。纯 Python 约 0.4 MB/s，评估工具中先用 imagecodecs 试，不一致的条带才用它。
"""
from __future__ import annotations

BITS_MIN, BITS_MAX = 9, 12
CODE_CLEAR, CODE_EOI, CODE_FIRST = 256, 257, 258
CODE_MAX = (1 << BITS_MAX) - 1
CHECK_GAP = 10000


def encode(data: bytes) -> bytes:
    out = bytearray()
    nextdata = nextbits = 0
    nbits = BITS_MIN
    maxcode = (1 << nbits) - 1
    outcount = 0

    def put(code: int):                     # libtiff 的 PutNextCode 宏
        nonlocal nextdata, nextbits, outcount
        nextdata = ((nextdata << nbits) | code) & 0xFFFFFFFF
        nextbits += nbits
        out.append((nextdata >> (nextbits - 8)) & 0xFF)
        nextbits -= 8
        if nextbits >= 8:
            out.append((nextdata >> (nextbits - 8)) & 0xFF)
            nextbits -= 8
        outcount += nbits

    if data:
        free_ent = CODE_FIRST
        checkpoint = CHECK_GAP
        ratio = 0
        table: dict[int, int] = {}
        put(CODE_CLEAR)
        ent = data[0]
        incount = 1
        for c in memoryview(data)[1:]:
            incount += 1
            key = (c << 12) | ent
            code = table.get(key)
            if code is not None:
                ent = code
                continue
            put(ent)
            ent = c
            table[key] = free_ent
            free_ent += 1
            reset = False
            if free_ent == CODE_MAX - 1:            # 码表满
                reset = True
            elif free_ent > maxcode:                # 码宽 +1
                nbits += 1
                maxcode = (1 << nbits) - 1
            elif incount >= checkpoint:             # 压缩率检查
                checkpoint = incount + CHECK_GAP
                if incount > 0x007FFFFF:
                    r = outcount >> 8
                    rat = 0x7FFFFFFF if r == 0 else incount // r
                else:
                    rat = (incount << 8) // outcount
                if rat <= ratio:
                    reset = True
                else:
                    ratio = rat
            if reset:
                table.clear()
                ratio = 0
                incount = outcount = 0
                free_ent = CODE_FIRST
                put(CODE_CLEAR)
                nbits = BITS_MIN
                maxcode = (1 << nbits) - 1
        # LZWPostEncode
        put(ent)
        free_ent += 1
        if free_ent == CODE_MAX - 1:
            outcount = 0
            put(CODE_CLEAR)
            nbits = BITS_MIN
        elif free_ent > maxcode:
            nbits += 1
    put(CODE_EOI)
    if nextbits > 0:
        out.append((nextdata << (8 - nextbits)) & 0xFF)
    return bytes(out)
