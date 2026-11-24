// 缩放 / 适应窗口 / 平移限制 / 状态栏文字:纯计算,不碰窗口。
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace xv {

// 适应窗口:图比窗口大且允许缩小 → 缩小到刚好放下;图比窗口小且允许放大 → 放大到刚好放下;否则 100%
inline double fitZoom(double imgW, double imgH, double viewW, double viewH, bool shrink, bool enlarge) {
    if (imgW <= 0 || imgH <= 0 || viewW <= 0 || viewH <= 0) return 1.0;
    double z = std::min(viewW / imgW, viewH / imgH);
    if (z < 1.0) return shrink ? z : 1.0;
    if (z > 1.0) return enlarge ? z : 1.0;
    return 1.0;
}

inline double clampZoom(double z, double zmin, double zmax) { return std::max(zmin, std::min(zmax, z)); }

// 平移范围:图比窗口大多少,就能往两边各移一半
inline double maxPan(double shownSize, double viewSize) { return std::max(0.0, (shownSize - viewSize) / 2.0); }

inline double clampPan(double off, double shownSize, double viewSize) {
    double m = maxPan(shownSize, viewSize);
    return std::max(-m, std::min(m, off));
}

// 以 anchor(相对视图中心的坐标)为不动点缩放后的新偏移
inline double zoomOffset(double anchorRel, double off, double oldZoom, double newZoom) {
    double rel = anchorRel - off;
    return anchorRel - rel * (newZoom / oldZoom);
}

inline const char* colorDepthText(int bits, char* buf, size_t n) {
    if (bits == 24 || bits == 32) std::snprintf(buf, n, "16M");
    else if (bits == 8) std::snprintf(buf, n, "256");
    else std::snprintf(buf, n, "%d", bits);
    return buf;
}

// "2000x3000x16M jpeg"
inline std::string infoLine(int w, int h, int bits, const std::string& fmt) {
    char d[16];
    colorDepthText(bits, d, sizeof d);
    char b[96];
    std::snprintf(b, sizeof b, "%dx%dx%s", w, h, d);
    std::string s = b;
    if (!fmt.empty()) s += " " + fmt;
    return s;
}

inline std::string sizeText(unsigned long long bytes) {
    char b[48];
    if (bytes < 1024ULL) std::snprintf(b, sizeof b, "%llu B", bytes);
    else if (bytes < 1024ULL * 1024) std::snprintf(b, sizeof b, "%.0f KB", bytes / 1024.0);
    else if (bytes < 1024ULL * 1024 * 1024) std::snprintf(b, sizeof b, "%.1f MB", bytes / 1048576.0);
    else std::snprintf(b, sizeof b, "%.2f GB", bytes / 1073741824.0);
    return b;
}

// 把 "%1 %2" 占位符换成参数
inline std::string fmtArgs(std::string tpl, const std::string& a1 = "", const std::string& a2 = "") {
    auto rep = [&](const char* key, const std::string& val) {
        size_t p = 0;
        while ((p = tpl.find(key, p)) != std::string::npos) { tpl.replace(p, 2, val); p += val.size(); }
    };
    rep("%1", a1);
    rep("%2", a2);
    return tpl;
}

// "\n" 这种两个字符的转义 → 真换行
inline std::string unescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') { o += '\n'; ++i; }
        else o += s[i];
    }
    return o;
}

// "#rrggbb" → 0xRRGGBB;不合法返回 def
inline unsigned parseHexColor(const std::string& s, unsigned def) {
    if (s.size() != 7 || s[0] != '#') return def;
    unsigned v = 0;
    for (size_t i = 1; i < 7; ++i) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return def;
    }
    return v;
}

}  // namespace xv
