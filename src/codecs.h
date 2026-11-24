// 不依赖 Windows 的图像解码:PSD/PSB 合成图、Radiance HDR、OpenEXR、相机 RAW 的内嵌 JPEG 预览,以及 deflate/gzip 解压。
// 输出统一是 "预乘 alpha 的 BGRA 8 位",main.cpp 直接包成 WIC 位图。
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace xcodec {

struct Image {
    int w = 0, h = 0;
    int bits = 24;                 // 24 / 32(有透明)
    std::string format;            // psd / hdr ...
    std::vector<uint8_t> bgra;     // 预乘 BGRA,w*h*4
};

// ------------------------------------------------------------------ 公共小工具
inline uint8_t srgbEncode(double v) {
    if (!(v > 0)) return 0;
    if (v >= 1) return 255;
    double g = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
    return (uint8_t)std::min(255.0, std::max(0.0, g * 255.0 + 0.5));
}

struct Reader {
    const uint8_t* d;
    size_t n;
    bool ok(size_t pos, size_t len) const { return pos <= n && len <= n - pos; }
    uint16_t u16(size_t p) const { return (uint16_t)((d[p] << 8) | d[p + 1]); }
    uint32_t u32(size_t p) const { return ((uint32_t)d[p] << 24) | ((uint32_t)d[p + 1] << 16) | ((uint32_t)d[p + 2] << 8) | d[p + 3]; }
    uint64_t u64(size_t p) const { return ((uint64_t)u32(p) << 32) | u32(p + 4); }
};

// ------------------------------------------------------------------ PSD / PSB (读 Photoshop 存的"合成整图")
// 支持:位深 1/8/16/32,模式 位图/灰度/索引/RGB/CMYK/Lab/双色调,压缩 原始/PackBits,PSB 大文档。
// 透明:只有图层段的层数为负数(= 合成图的第一个 alpha 通道是透明度)时才当透明,和 Photoshop 一致。
// 没勾"最大兼容性"的 PSD 合成图是空白的,这里读出来就是空白(需要按图层合成,另说)。
inline bool decodePsd(const uint8_t* data, size_t n, Image& out, std::string& err) {
    Reader R{data, n};
    if (n < 26 || std::memcmp(data, "8BPS", 4) != 0) { err = "不是 PSD 文件"; return false; }
    int ver = R.u16(4);
    if (ver != 1 && ver != 2) { err = "不认识的 PSD 版本"; return false; }
    int chans = R.u16(12);
    uint32_t H = R.u32(14), W = R.u32(18);
    int depth = R.u16(22), mode = R.u16(24);
    if (W == 0 || H == 0 || W > 300000 || H > 300000 || (ver == 1 && (W > 30000 || H > 30000))) { err = "PSD 尺寸不合理"; return false; }
    if (chans < 1 || chans > 56) { err = "PSD 通道数不合理"; return false; }
    if (depth != 1 && depth != 8 && depth != 16 && depth != 32) { err = "不支持的 PSD 位深"; return false; }
    int need = 0;
    switch (mode) {
        case 0: case 1: case 2: case 8: need = 1; break;
        case 3: case 9: need = 3; break;
        case 4: need = 4; break;
        default: err = "不支持的 PSD 颜色模式(多通道等)"; return false;
    }
    if (chans < need) { err = "PSD 通道数少于颜色模式需要的"; return false; }

    size_t pos = 26;
    if (!R.ok(pos, 4)) { err = "PSD 文件被截断"; return false; }
    uint32_t colLen = R.u32(pos);
    size_t colPos = pos + 4;
    if (!R.ok(colPos, colLen)) { err = "PSD 文件被截断"; return false; }
    pos = colPos + colLen;
    if (!R.ok(pos, 4)) { err = "PSD 文件被截断"; return false; }
    pos += 4 + (size_t)R.u32(pos);                       // 图像资源段:跳过
    size_t lsz = ver == 1 ? 4 : 8;
    if (!R.ok(pos, lsz)) { err = "PSD 文件被截断"; return false; }
    uint64_t layerLen = ver == 1 ? R.u32(pos) : R.u64(pos);
    size_t layerStart = pos + lsz;
    bool mergedAlpha = false;
    if (layerLen >= lsz + 2 && R.ok(layerStart, lsz + 2)) {   // 层信息:长度 + 层数(int16,负数 = 合成图带透明)
        uint64_t liLen = ver == 1 ? R.u32(layerStart) : R.u64(layerStart);
        if (liLen >= 2 && R.ok(layerStart + lsz, 2)) mergedAlpha = (int16_t)R.u16(layerStart + lsz) < 0;
    }
    if (layerLen > n || !R.ok(layerStart, (size_t)layerLen)) { err = "PSD 文件被截断"; return false; }
    pos = layerStart + (size_t)layerLen;
    if (!R.ok(pos, 2)) { err = "PSD 没有合成图数据"; return false; }
    int comp = R.u16(pos);
    pos += 2;
    if (comp != 0 && comp != 1) { err = "不支持的 PSD 压缩方式(ZIP)"; return false; }

    bool hasAlpha = mergedAlpha && chans > need && (mode == 1 || mode == 3 || mode == 4);
    int use = need + (hasAlpha ? 1 : 0);
    size_t plane = (size_t)W * H;
    if (plane * (size_t)use > (size_t)1600 * 1000 * 1000) { err = "PSD 太大,内存放不下"; return false; }
    size_t rowBytes = ((size_t)W * depth + 7) / 8;
    int bps = depth == 16 ? 2 : depth == 32 ? 4 : 1;

    std::vector<std::vector<uint8_t>> pl((size_t)use);
    for (auto& p : pl) p.assign(plane, 0);

    // 一行原始样本 -> 8 位
    auto toPlane = [&](const uint8_t* row, uint8_t* dst, bool lab) {
        if (depth == 8) { std::memcpy(dst, row, W); }
        else if (depth == 16) {
            for (uint32_t x = 0; x < W; ++x) { uint32_t v = (uint32_t)(row[2 * x] << 8) | row[2 * x + 1]; dst[x] = (uint8_t)((v * 255 + 32767) / 65535); }
        } else if (depth == 32) {
            for (uint32_t x = 0; x < W; ++x) {
                uint32_t u = ((uint32_t)row[4 * x] << 24) | ((uint32_t)row[4 * x + 1] << 16) | ((uint32_t)row[4 * x + 2] << 8) | row[4 * x + 3];
                float f; std::memcpy(&f, &u, 4);
                dst[x] = lab ? (uint8_t)std::min(255.0, std::max(0.0, f * 255.0 + 0.5)) : srgbEncode(f);   // 32 位是线性光
            }
        } else {
            for (uint32_t x = 0; x < W; ++x) dst[x] = (row[x >> 3] >> (7 - (x & 7))) & 1 ? 0 : 255;       // 位图:1 = 黑
        }
    };
    bool lab = mode == 9;

    if (comp == 0) {
        size_t need_bytes = rowBytes * H * (size_t)chans;
        if (!R.ok(pos, need_bytes)) { err = "PSD 文件被截断"; return false; }
        for (int c = 0; c < use; ++c)
            for (uint32_t y = 0; y < H; ++y)
                toPlane(data + pos + ((size_t)c * H + y) * rowBytes, pl[(size_t)c].data() + (size_t)y * W, lab);
    } else {
        size_t cs = ver == 1 ? 2 : 4;
        size_t tbl = (size_t)chans * H * cs;
        if (!R.ok(pos, tbl)) { err = "PSD 文件被截断"; return false; }
        size_t dpos = pos + tbl, ti = pos;
        std::vector<uint8_t> row(rowBytes);
        for (int c = 0; c < chans; ++c) {
            for (uint32_t y = 0; y < H; ++y) {
                size_t clen = cs == 2 ? R.u16(ti) : R.u32(ti);
                ti += cs;
                if (!R.ok(dpos, clen)) { err = "PSD 文件被截断"; return false; }
                if (c < use) {
                    size_t o = 0, i = 0;
                    const uint8_t* s = data + dpos;
                    while (i < clen && o < rowBytes) {
                        int8_t b = (int8_t)s[i++];
                        if (b >= 0) {
                            size_t cnt = (size_t)b + 1;
                            if (i + cnt > clen) cnt = clen - i;
                            if (o + cnt > rowBytes) cnt = rowBytes - o;
                            std::memcpy(row.data() + o, s + i, cnt);
                            i += (size_t)b + 1; o += cnt;
                        } else if (b != -128) {
                            size_t cnt = (size_t)(1 - b);
                            if (i >= clen) break;
                            uint8_t v = s[i++];
                            if (o + cnt > rowBytes) cnt = rowBytes - o;
                            std::memset(row.data() + o, v, cnt);
                            o += cnt;
                        }
                    }
                    if (o < rowBytes) std::memset(row.data() + o, 0, rowBytes - o);
                    toPlane(row.data(), pl[(size_t)c].data() + (size_t)y * W, lab);
                }
                dpos += clen;
            }
        }
    }
    (void)bps;

    out.w = (int)W; out.h = (int)H; out.format = "psd";
    out.bits = hasAlpha ? 32 : 24;
    out.bgra.assign(plane * 4, 0);
    uint8_t pal[768] = {0};
    if (mode == 2 && colLen >= 768) std::memcpy(pal, data + colPos, 768);

    auto finv = [](double t) { return t > 6.0 / 29 ? t * t * t : 3 * (6.0 / 29) * (6.0 / 29) * (t - 4.0 / 29); };
    for (size_t i = 0; i < plane; ++i) {
        uint8_t r, g, b;
        switch (mode) {
            case 3: r = pl[0][i]; g = pl[1][i]; b = pl[2][i]; break;
            case 4: {   // Photoshop 的 CMYK 是反相存的(255 = 没墨)
                uint32_t k = pl[3][i];
                r = (uint8_t)(pl[0][i] * k / 255); g = (uint8_t)(pl[1][i] * k / 255); b = (uint8_t)(pl[2][i] * k / 255);
                break;
            }
            case 2: r = pal[pl[0][i]]; g = pal[256 + pl[0][i]]; b = pal[512 + pl[0][i]]; break;
            case 9: {
                double L = pl[0][i] * 100.0 / 255.0, a = (double)pl[1][i] - 128.0, bb = (double)pl[2][i] - 128.0;
                double fy = (L + 16) / 116, fx = fy + a / 500, fz = fy - bb / 200;
                double X = 0.9642 * finv(fx), Y = finv(fy), Z = 0.8249 * finv(fz);
                r = srgbEncode(3.1338561 * X - 1.6168667 * Y - 0.4906146 * Z);
                g = srgbEncode(-0.9787684 * X + 1.9161415 * Y + 0.0334540 * Z);
                b = srgbEncode(0.0719453 * X - 0.2289914 * Y + 1.4052427 * Z);
                break;
            }
            default: r = g = b = pl[0][i];
        }
        uint32_t a = hasAlpha ? pl[(size_t)need][i] : 255;
        uint8_t* o = &out.bgra[i * 4];
        if (a == 255) { o[0] = b; o[1] = g; o[2] = r; o[3] = 255; }
        else { o[0] = (uint8_t)((b * a + 127) / 255); o[1] = (uint8_t)((g * a + 127) / 255); o[2] = (uint8_t)((r * a + 127) / 255); o[3] = (uint8_t)a; }
    }
    return true;
}

// ------------------------------------------------------------------ Radiance HDR(RGBE)
// 读成线性后:自动曝光(对数平均亮度 -> 0.18)+ ACES 曲线 + sRGB,和原来 Python 版一致。
inline bool decodeHdr(const uint8_t* d, size_t n, Image& out, std::string& err) {
    if (n < 16 || (std::memcmp(d, "#?RADIANCE", 10) != 0 && std::memcmp(d, "#?RGBE", 6) != 0)) { err = "不是 Radiance HDR 文件"; return false; }
    size_t pos = 0;
    for (;;) {   // 头部以空行结束
        size_t nl = pos;
        while (nl < n && d[nl] != '\n') ++nl;
        if (nl >= n) { err = "HDR 文件头不完整"; return false; }
        std::string line((const char*)d + pos, nl - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        pos = nl + 1;
        std::string up = line;
        for (auto& c : up) c = (char)std::toupper((unsigned char)c);
        if (up.rfind("FORMAT=", 0) == 0 && up.find("RGBE") == std::string::npos) { err = "不支持的 HDR 格式(只认 RGBE)"; return false; }
        if (line.empty()) break;
    }
    size_t nl = pos;
    while (nl < n && d[nl] != '\n') ++nl;
    if (nl >= n) { err = "HDR 缺少尺寸行"; return false; }
    std::string res((const char*)d + pos, nl - pos);
    pos = nl + 1;
    char sy = 0, sx = 0; int H = 0, W = 0;
    if (std::sscanf(res.c_str(), " %cY %d %cX %d", &sy, &H, &sx, &W) != 4 || (sy != '+' && sy != '-') || (sx != '+' && sx != '-')) { err = "HDR 的方向写法不支持"; return false; }
    if (W <= 0 || H <= 0 || W > 100000 || H > 100000 || (size_t)W * H > (size_t)400 * 1000 * 1000) { err = "HDR 尺寸不合理"; return false; }

    std::vector<uint8_t> px((size_t)W * H * 4);   // RGBE,最后原地转成 BGRA
    for (int y = 0; y < H; ++y) {
        uint8_t* row = &px[(size_t)y * W * 4];
        if (pos + 4 > n) { err = "HDR 数据被截断"; return false; }
        bool rle = W >= 8 && W < 32768 && d[pos] == 2 && d[pos + 1] == 2 && !(d[pos + 2] & 0x80) && (((d[pos + 2] << 8) | d[pos + 3]) == W);
        if (rle) {
            pos += 4;
            for (int c = 0; c < 4; ++c) {
                int x = 0;
                while (x < W) {
                    if (pos >= n) { err = "HDR 数据被截断"; return false; }
                    int cnt = d[pos++];
                    if (cnt > 128) {
                        cnt -= 128;
                        if (pos >= n || x + cnt > W) { err = "HDR 数据损坏"; return false; }
                        uint8_t v = d[pos++];
                        while (cnt--) row[(size_t)(x++) * 4 + c] = v;
                    } else {
                        if (cnt == 0 || pos + cnt > n || x + cnt > W) { err = "HDR 数据损坏"; return false; }
                        while (cnt--) row[(size_t)(x++) * 4 + c] = d[pos++];
                    }
                }
            }
        } else {   // 平铺 + 旧式游程(1,1,1,n)
            int x = 0, shift = 0;
            while (x < W) {
                if (pos + 4 > n) { err = "HDR 数据被截断"; return false; }
                uint8_t* q = row + (size_t)x * 4;
                std::memcpy(q, d + pos, 4);
                pos += 4;
                if (q[0] == 1 && q[1] == 1 && q[2] == 1) {
                    if (x == 0) { err = "HDR 数据损坏"; return false; }
                    int cnt = q[3] << shift;
                    for (int k = 0; k < cnt && x < W; ++k, ++x) std::memcpy(row + (size_t)x * 4, row + (size_t)(x - 1) * 4, 4);
                    shift += 8;
                } else { ++x; shift = 0; }
            }
        }
    }
    auto lin = [](uint8_t m, int e) { return e == 0 ? 0.0 : (m + 0.5) * std::ldexp(1.0, e - 136); };
    // 自动曝光:在抽样上算对数平均亮度
    int k = std::max(1, std::max(W, H) / 256);
    double sum = 0; size_t cnt = 0;
    for (int y = 0; y < H; y += k)
        for (int x = 0; x < W; x += k) {
            const uint8_t* q = &px[((size_t)y * W + x) * 4];
            double r = lin(q[0], q[3]), g = lin(q[1], q[3]), b = lin(q[2], q[3]);
            double l = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            if (std::isfinite(l) && l > 0) { sum += std::log(l + 1e-4); ++cnt; }
        }
    double scale = 1.0;
    if (cnt) { double key = std::exp(sum / cnt); scale = std::min(std::max(0.18 / std::max(key, 1e-6), 0.05), 50.0); }
    static const int N = 16384;
    std::vector<uint8_t> lut(N);
    for (int i = 0; i < N; ++i) lut[i] = srgbEncode((double)i / (N - 1));
    auto tm = [&](double v) {
        v *= scale;
        if (!(v > 0)) return (uint8_t)0;
        double y = (v * (2.51 * v + 0.03)) / (v * (2.43 * v + 0.59) + 0.14);
        y = std::min(1.0, std::max(0.0, y));
        return lut[(int)(y * (N - 1) + 0.5)];
    };
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        uint8_t* q = &px[i * 4];
        int e = q[3];
        uint8_t r = tm(lin(q[0], e)), g = tm(lin(q[1], e)), b = tm(lin(q[2], e));
        q[0] = b; q[1] = g; q[2] = r; q[3] = 255;
    }
    if (sy == '+') for (int y = 0; y < H / 2; ++y) std::swap_ranges(&px[(size_t)y * W * 4], &px[(size_t)(y + 1) * W * 4], &px[(size_t)(H - 1 - y) * W * 4]);
    if (sx == '-') for (int y = 0; y < H; ++y) for (int x = 0; x < W / 2; ++x) std::swap_ranges(&px[((size_t)y * W + x) * 4], &px[((size_t)y * W + x) * 4 + 4], &px[((size_t)y * W + W - 1 - x) * 4]);
    out.w = W; out.h = H; out.bits = 24; out.format = "hdr";
    out.bgra.swap(px);
    return true;
}

// ------------------------------------------------------------------ 解压:deflate / zlib / gzip(EXR 的 ZIP 压缩和 .svgz 要用,不依赖 zlib 库)
struct InflBits {
    const uint8_t* d; size_t n, pos = 0; uint32_t buf = 0; int cnt = 0; bool bad = false;
    int bit() {
        if (cnt == 0) { if (pos >= n) { bad = true; return 0; } buf = d[pos++]; cnt = 8; }
        int b = (int)(buf & 1); buf >>= 1; --cnt; return b;
    }
    int bits(int need) { int v = 0; for (int i = 0; i < need; ++i) v |= bit() << i; return v; }
};
struct InflHuff { uint16_t count[16]; uint16_t symbol[320]; };
inline int inflBuild(InflHuff& h, const uint16_t* len, int n) {
    for (int i = 0; i < 16; ++i) h.count[i] = 0;
    for (int i = 0; i < n; ++i) h.count[len[i]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int l = 1; l < 16; ++l) { left <<= 1; left -= h.count[l]; if (left < 0) return left; }
    uint16_t offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; ++l) offs[l + 1] = (uint16_t)(offs[l] + h.count[l]);
    for (int s = 0; s < n; ++s) if (len[s]) h.symbol[offs[len[s]]++] = (uint16_t)s;
    return left;
}
inline int inflDecode(InflBits& b, const InflHuff& h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; ++l) {
        code |= b.bit();
        if (b.bad) return -1;
        int count = h.count[l];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}
// 解 raw deflate 数据。maxOut 是输出上限(防压缩炸弹)。
inline bool inflateRaw(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t maxOut) {
    static const uint16_t lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const uint16_t lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const uint16_t dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    InflBits b{src, n};
    out.clear();
    int last;
    do {
        last = b.bit();
        int type = b.bits(2);
        if (b.bad) return false;
        if (type == 0) {
            b.cnt = 0;   // 丢掉当前字节剩余的位
            if (b.pos + 4 > n) return false;
            unsigned len = src[b.pos] | (src[b.pos + 1] << 8), nlen = src[b.pos + 2] | (src[b.pos + 3] << 8);
            b.pos += 4;
            if ((len ^ 0xFFFF) != nlen || b.pos + len > n || out.size() + len > maxOut) return false;
            out.insert(out.end(), src + b.pos, src + b.pos + len);
            b.pos += len;
        } else if (type == 1 || type == 2) {
            InflHuff lh, dh;
            uint16_t lengths[320];
            if (type == 1) {
                int i = 0;
                for (; i < 144; ++i) lengths[i] = 8;
                for (; i < 256; ++i) lengths[i] = 9;
                for (; i < 280; ++i) lengths[i] = 7;
                for (; i < 288; ++i) lengths[i] = 8;
                inflBuild(lh, lengths, 288);
                for (i = 0; i < 30; ++i) lengths[i] = 5;
                inflBuild(dh, lengths, 30);
            } else {
                int nlen = b.bits(5) + 257, ndist = b.bits(5) + 1, ncode = b.bits(4) + 4;
                if (b.bad || nlen > 286 || ndist > 30) return false;
                static const int order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
                uint16_t cl[19] = {0};
                for (int i = 0; i < ncode; ++i) cl[order[i]] = (uint16_t)b.bits(3);
                InflHuff ch;
                if (inflBuild(ch, cl, 19) != 0) return false;
                int idx = 0;
                while (idx < nlen + ndist) {
                    int sym = inflDecode(b, ch);
                    if (sym < 0) return false;
                    if (sym < 16) lengths[idx++] = (uint16_t)sym;
                    else {
                        int rep, val = 0;
                        if (sym == 16) { if (idx == 0) return false; val = lengths[idx - 1]; rep = 3 + b.bits(2); }
                        else if (sym == 17) rep = 3 + b.bits(3);
                        else rep = 11 + b.bits(7);
                        if (b.bad || idx + rep > nlen + ndist) return false;
                        while (rep--) lengths[idx++] = (uint16_t)val;
                    }
                }
                if (lengths[256] == 0) return false;
                int e = inflBuild(lh, lengths, nlen);
                if (e < 0 || (e > 0 && nlen - lh.count[0] != 1)) return false;
                e = inflBuild(dh, lengths + nlen, ndist);
                if (e < 0 || (e > 0 && ndist - dh.count[0] != 1)) return false;
            }
            for (;;) {
                int sym = inflDecode(b, lh);
                if (sym < 0 || b.bad) return false;
                if (sym < 256) {
                    if (out.size() >= maxOut) return false;
                    out.push_back((uint8_t)sym);
                } else if (sym == 256) break;
                else {
                    sym -= 257;
                    if (sym >= 29) return false;
                    int len = lbase[sym] + b.bits(lext[sym]);
                    int ds = inflDecode(b, dh);
                    if (ds < 0 || ds >= 30) return false;
                    size_t dist = dbase[ds] + (size_t)b.bits(dext[ds]);
                    if (b.bad || dist > out.size() || out.size() + len > maxOut) return false;
                    size_t from = out.size() - dist;
                    for (int i = 0; i < len; ++i) out.push_back(out[from + i]);
                }
            }
        } else return false;
    } while (!last);
    return true;
}
inline bool inflateZlib(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t maxOut) {
    if (n < 3 || (src[0] & 0x0F) != 8 || ((src[0] << 8) | src[1]) % 31 != 0 || (src[1] & 0x20)) return false;
    return inflateRaw(src + 2, n - 2, out, maxOut);
}
inline bool gunzip(const uint8_t* d, size_t n, std::vector<uint8_t>& out, size_t maxOut) {
    if (n < 18 || d[0] != 0x1F || d[1] != 0x8B || d[2] != 8) return false;
    int flg = d[3];
    size_t p = 10;
    if (flg & 4) { if (p + 2 > n) return false; p += 2 + (d[p] | (d[p + 1] << 8)); }
    if (flg & 8) { while (p < n && d[p]) ++p; ++p; }
    if (flg & 16) { while (p < n && d[p]) ++p; ++p; }
    if (flg & 2) p += 2;
    if (p >= n) return false;
    return inflateRaw(d + p, n - p, out, maxOut);
}

// ------------------------------------------------------------------ OpenEXR(扫描线、单部分)
// 压缩:无 / RLE / ZIPS / ZIP;像素:half / float / uint。读 R G B(A)或单通道 Y。
// 不支持:PIZ PXR24 B44 DWA(会明确提示)、平铺、深度图、多部分。
// 显示:和 HDR 一样 自动曝光 + ACES + sRGB。
inline float halfToFloat(uint16_t h) {
    int s = h >> 15, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = std::ldexp((float)m, -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp((float)(m + 1024), e - 25);
    return s ? -v : v;
}

inline bool decodeExr(const uint8_t* d, size_t n, Image& out, std::string& err) {
    if (n < 16 || d[0] != 0x76 || d[1] != 0x2F || d[2] != 0x31 || d[3] != 0x01) { err = "不是 EXR 文件"; return false; }
    auto le32 = [&](size_t p) { return (uint32_t)d[p] | ((uint32_t)d[p + 1] << 8) | ((uint32_t)d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24); };
    auto le64 = [&](size_t p) { return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); };
    uint32_t flags = le32(4);
    if (flags & 0x200) { err = "暂不支持平铺的 EXR"; return false; }
    if (flags & 0x1800) { err = "暂不支持深度图 / 多部分的 EXR"; return false; }
    struct Chan { std::string name; int type = 1, xs = 1, ys = 1; size_t off = 0; };
    std::vector<Chan> ch;
    int comp = -1, x0 = 0, y0 = 0, x1 = -1, y1 = -1;
    size_t pos = 8;
    for (;;) {
        size_t s = pos;
        while (pos < n && d[pos]) ++pos;
        if (pos >= n || pos - s > 255) { err = "EXR 文件头损坏"; return false; }
        std::string name((const char*)d + s, pos - s);
        ++pos;
        if (name.empty()) break;
        s = pos;
        while (pos < n && d[pos]) ++pos;
        if (pos >= n || pos - s > 255) { err = "EXR 文件头损坏"; return false; }
        std::string type((const char*)d + s, pos - s);
        ++pos;
        if (pos + 4 > n) { err = "EXR 文件头损坏"; return false; }
        size_t sz = le32(pos);
        pos += 4;
        if (sz > n - pos) { err = "EXR 文件头损坏"; return false; }
        size_t a = pos;
        if (name == "channels") {
            size_t p = a, end = a + sz;
            for (;;) {
                size_t q = p;
                while (q < end && d[q]) ++q;
                if (q >= end) break;
                if (q == p) break;
                Chan c;
                c.name.assign((const char*)d + p, q - p);
                p = q + 1;
                if (p + 16 > end) { err = "EXR 通道表损坏"; return false; }
                c.type = (int)le32(p); c.xs = (int)le32(p + 8); c.ys = (int)le32(p + 12);
                p += 16;
                if (c.type < 0 || c.type > 2 || c.xs < 1 || c.ys < 1) { err = "EXR 通道表损坏"; return false; }
                ch.push_back(c);
            }
        } else if (name == "compression" && sz >= 1) comp = d[a];
        else if (name == "dataWindow" && sz >= 16) { x0 = (int)le32(a); y0 = (int)le32(a + 4); x1 = (int)le32(a + 8); y1 = (int)le32(a + 12); }
        pos += sz;
    }
    if (ch.empty() || comp < 0 || x1 < x0 || y1 < y0) { err = "EXR 缺少必要的头信息"; return false; }
    static const char* cnames[] = {"无压缩", "RLE", "ZIPS", "ZIP", "PIZ", "PXR24", "B44", "B44A", "DWAA", "DWAB"};
    if (comp > 3) { err = std::string("暂不支持 EXR 的 ") + (comp < 10 ? cnames[comp] : "未知") + " 压缩"; return false; }
    int64_t W64 = (int64_t)x1 - x0 + 1, H64 = (int64_t)y1 - y0 + 1;
    if (W64 > 65536 || H64 > 65536 || W64 * H64 > 80000000) { err = "EXR 尺寸不合理或太大"; return false; }
    int W = (int)W64, H = (int)H64;
    // 每条扫描线里各通道的位置(通道表是按名字排好序的)
    size_t pbytes[3] = {4, 2, 4}, scan = 0;
    for (auto& c : ch) { c.off = scan; scan += (size_t)W * pbytes[c.type]; }
    auto find = [&](const char* nm) -> int {
        int sfx = -1;
        std::string dot = std::string(".") + nm;
        for (size_t i = 0; i < ch.size(); ++i) {
            if (ch[i].name == nm) return (int)i;
            if (sfx < 0 && ch[i].name.size() > dot.size() && ch[i].name.compare(ch[i].name.size() - dot.size(), dot.size(), dot) == 0) sfx = (int)i;
        }
        return sfx;
    };
    int cr = find("R"), cg = find("G"), cb = find("B"), ca = find("A"), cy = -1;
    if (cr < 0 || cg < 0 || cb < 0) { cy = find("Y"); if (cy < 0) { err = "EXR 里没有 R G B 或 Y 通道"; return false; } }
    int use[4] = {cr, cg, cb, ca};
    if (cy >= 0) { use[0] = use[1] = use[2] = cy; }
    for (int u : use) if (u >= 0 && (ch[u].xs != 1 || ch[u].ys != 1)) { err = "暂不支持带下采样通道的 EXR"; return false; }
    int lpb = comp == 3 ? 16 : 1;
    size_t nblocks = ((size_t)H + lpb - 1) / lpb;
    if (pos + nblocks * 8 > n) { err = "EXR 偏移表被截断"; return false; }
    size_t table = pos;
    const bool hasA = use[3] >= 0;
    std::vector<float> px;
    try { px.assign((size_t)W * H * 4, 0.0f); } catch (...) { err = "内存不足,无法读取这张 EXR"; return false; }
    if (!hasA) for (size_t i = 3; i < px.size(); i += 4) px[i] = 1.0f;
    std::vector<uint8_t> raw, tmp;
    bool any = false;
    for (size_t b = 0; b < nblocks; ++b) {
        uint64_t off = le64(table + b * 8);
        if (off == 0 || off > n || n - off < 8) continue;   // 没写完的文件:这一段留黑
        int y = (int)le32((size_t)off);
        size_t csz = le32((size_t)off + 4);
        if (csz > n - (size_t)off - 8) continue;
        int ys = y - y0;
        if (ys < 0 || ys >= H) continue;
        int lines = std::min(lpb, H - ys);
        size_t want = (size_t)lines * scan;
        const uint8_t* cp = d + off + 8;
        const uint8_t* block = cp;
        if (comp != 0 && csz != want) {
            tmp.clear();
            if (comp == 3 || comp == 2) { if (!inflateZlib(cp, csz, tmp, want) || tmp.size() != want) { err = "EXR 的 ZIP 数据损坏"; return false; } }
            else {   // RLE
                size_t i = 0;
                while (i < csz) {
                    int c = (int8_t)cp[i++];
                    if (c < 0) { size_t k = (size_t)-c; if (i + k > csz || tmp.size() + k > want) { err = "EXR 的 RLE 数据损坏"; return false; } tmp.insert(tmp.end(), cp + i, cp + i + k); i += k; }
                    else { size_t k = (size_t)c + 1; if (i >= csz || tmp.size() + k > want) { err = "EXR 的 RLE 数据损坏"; return false; } tmp.insert(tmp.end(), k, cp[i++]); }
                }
                if (tmp.size() != want) { err = "EXR 的 RLE 数据损坏"; return false; }
            }
            for (size_t i = 1; i < want; ++i) tmp[i] = (uint8_t)(tmp[i - 1] + tmp[i] - 128);   // 预测器
            raw.resize(want);
            size_t half = (want + 1) / 2;
            for (size_t k = 0, i = 0; i < want; ++k) { raw[i++] = tmp[k]; if (i < want) raw[i++] = tmp[half + k]; }
            block = raw.data();
        } else if (csz != want) { err = "EXR 数据长度不对"; return false; }
        for (int l = 0; l < lines; ++l) {
            const uint8_t* line = block + (size_t)l * scan;
            float* dst = &px[((size_t)(ys + l) * W) * 4];
            for (int k = 0; k < 4; ++k) {
                if (use[k] < 0) continue;
                const Chan& c = ch[use[k]];
                const uint8_t* s = line + c.off;
                for (int x = 0; x < W; ++x) {
                    float v;
                    if (c.type == 1) v = halfToFloat((uint16_t)(s[x * 2] | (s[x * 2 + 1] << 8)));
                    else if (c.type == 2) { uint32_t u = (uint32_t)s[x * 4] | ((uint32_t)s[x * 4 + 1] << 8) | ((uint32_t)s[x * 4 + 2] << 16) | ((uint32_t)s[x * 4 + 3] << 24); std::memcpy(&v, &u, 4); }
                    else { uint32_t u = (uint32_t)s[x * 4] | ((uint32_t)s[x * 4 + 1] << 8) | ((uint32_t)s[x * 4 + 2] << 16) | ((uint32_t)s[x * 4 + 3] << 24); v = (float)u; }
                    dst[(size_t)x * 4 + k] = v;
                }
            }
        }
        any = true;
    }
    if (!any) { err = "EXR 里没有读到任何像素数据"; return false; }
    // 自动曝光 + ACES + sRGB(同 HDR)
    int k = std::max(1, std::max(W, H) / 256);
    double sum = 0; size_t cnt = 0;
    for (int y = 0; y < H; y += k)
        for (int x = 0; x < W; x += k) {
            const float* q = &px[((size_t)y * W + x) * 4];
            double l = 0.2126 * q[0] + 0.7152 * q[1] + 0.0722 * q[2];
            if (std::isfinite(l) && l > 0) { sum += std::log(l + 1e-4); ++cnt; }
        }
    double scale = 1.0;
    if (cnt) { double key = std::exp(sum / cnt); scale = std::min(std::max(0.18 / std::max(key, 1e-6), 0.05), 50.0); }
    static const int N = 16384;
    std::vector<uint8_t> lut(N);
    for (int i = 0; i < N; ++i) lut[i] = srgbEncode((double)i / (N - 1));
    auto tm = [&](double v) {
        if (!std::isfinite(v)) return (uint8_t)(v > 0 ? 255 : 0);
        v *= scale;
        if (!(v > 0)) return (uint8_t)0;
        double y = (v * (2.51 * v + 0.03)) / (v * (2.43 * v + 0.59) + 0.14);
        y = std::min(1.0, std::max(0.0, y));
        return lut[(int)(y * (N - 1) + 0.5)];
    };
    out.bgra.assign((size_t)W * H * 4, 0);
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        const float* q = &px[i * 4];
        uint8_t* o = &out.bgra[i * 4];
        double al = std::isfinite(q[3]) ? std::min(1.0, std::max(0.0, (double)q[3])) : 1.0;
        uint8_t a = (uint8_t)(al * 255.0 + 0.5);
        o[0] = std::min(tm(q[2]), a); o[1] = std::min(tm(q[1]), a); o[2] = std::min(tm(q[0]), a); o[3] = a;   // EXR 的颜色本来就是预乘的
    }
    out.w = W; out.h = H; out.bits = hasA ? 32 : 24; out.format = "exr";
    return true;
}

// ------------------------------------------------------------------ 相机 RAW:取里面最大的内嵌 JPEG 预览
// CR2 CR3 NEF ARW DNG ORF RW2 RAF PEF SRW ... 都把整幅(或接近整幅)的 JPEG 预览塞在文件里。
// 不去解析各家的目录结构:找出所有 JPEG,按标记逐段走到结尾,取像素最多的那张。
struct JpegSpan { size_t off = 0, len = 0; uint64_t area = 0; };

inline bool jpegExtent(const uint8_t* d, size_t n, size_t i, size_t& end, uint64_t& area) {
    size_t p = i + 2;
    area = 0;
    for (;;) {
        while (p < n && d[p] != 0xFF) ++p;
        while (p < n && d[p] == 0xFF) ++p;
        if (p >= n) return false;
        uint8_t m = d[p++];
        if (m == 0xD9) { end = p; return area > 0; }
        if (m == 0x00 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
        if (p + 2 > n) return false;
        size_t len = ((size_t)d[p] << 8) | d[p + 1];
        if (len < 2 || p + len > n) return false;
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC && len >= 8) {
            uint64_t h = ((uint64_t)d[p + 3] << 8) | d[p + 4], w = ((uint64_t)d[p + 5] << 8) | d[p + 6];
            if (area == 0) area = w * h;
        }
        p += len;
        if (m == 0xDA) {   // 熵编码数据:里面的 FF 都带 00 / RSTn,遇到别的标记就回到逐段解析
            while (p + 1 < n) {
                if (d[p] == 0xFF && d[p + 1] != 0x00 && !(d[p + 1] >= 0xD0 && d[p + 1] <= 0xD7) && d[p + 1] != 0xFF) break;
                ++p;
            }
            if (p + 1 >= n) return false;
        }
    }
}

inline bool findRawPreview(const uint8_t* d, size_t n, size_t& off, size_t& len) {
    JpegSpan best;
    for (size_t i = 0; i + 3 < n; ++i) {
        if (d[i] == 0xFF && d[i + 1] == 0xD8 && d[i + 2] == 0xFF) {
            size_t end; uint64_t area;
            if (jpegExtent(d, n, i, end, area) && end - i > 2000 && area > best.area) { best.off = i; best.len = end - i; best.area = area; }
        }
    }
    if (best.area == 0) return false;
    off = best.off; len = best.len;
    return true;
}

// TIFF 系 RAW 的第一个目录里的 Orientation(0x0112);没有或不是 TIFF 容器返回 1
inline int rawOrientation(const uint8_t* d, size_t n) {
    if (n < 16) return 1;
    bool le = d[0] == 'I' && d[1] == 'I', be = d[0] == 'M' && d[1] == 'M';
    if (!le && !be) return 1;
    auto u16 = [&](size_t p) { return le ? (int)(d[p] | (d[p + 1] << 8)) : (int)((d[p] << 8) | d[p + 1]); };
    auto u32 = [&](size_t p) { return le ? (size_t)(d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | ((size_t)d[p + 3] << 24)) : (size_t)(((size_t)d[p] << 24) | (d[p + 1] << 16) | (d[p + 2] << 8) | d[p + 3]); };
    size_t ifd = u32(4);
    if (ifd + 2 > n) return 1;
    int cnt = u16(ifd);
    for (int i = 0; i < cnt; ++i) {
        size_t e = ifd + 2 + (size_t)i * 12;
        if (e + 12 > n) break;
        if (u16(e) == 0x0112) { int v = u16(e + 8); return v >= 1 && v <= 8 ? v : 1; }
    }
    return 1;
}

}  // namespace xcodec
