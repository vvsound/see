// 极简 SVG 解析:只认图标里用到的那几种元素(path/circle/ellipse/line/rect/polyline/polygon),
// 全部转成 "绝对坐标的线段/曲线/圆弧" 指令序列;真正画图在 main.cpp 里用 Direct2D。不依赖 windows.h,可单独测试。
#pragma once
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace xv {

struct PathCmd {
    enum Op { Move, Line, Cubic, Arc, Close } op = Move;
    double x = 0, y = 0;                  // 终点
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;  // 三次曲线控制点
    double rx = 0, ry = 0, rot = 0;       // 圆弧:半径、旋转
    bool large = false, sweep = false;
};

using Path = std::vector<PathCmd>;

struct SvgIcon {
    double vbX = 0, vbY = 0, vbW = 24, vbH = 24;
    double strokeWidth = 2;
    bool fill = false;          // 图标是否实心(fill 不是 none)
    bool hasStroke = false;     // 图标文件自带描边颜色(Icons/Tint=0 时按它画)
    unsigned stroke = 0;        // 0xRRGGBB
    std::vector<Path> paths;    // 每个元素一条(可含多个子路径)
};

namespace detail {

struct Scanner {
    const char* p;
    const char* e;
    void skip() { while (p < e && (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    bool more() { skip(); return p < e; }
    bool isCmd() { skip(); return p < e && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')); }
    bool num(double& v) {
        skip();
        if (p >= e) return false;
        char* end = nullptr;
        v = std::strtod(p, &end);
        if (end == p) return false;
        p = end;
        return true;
    }
    bool flag(bool& v) {  // 圆弧的标志位:单个 0/1,可能和后面的数字连在一起("a2 2 0 011 1")
        skip();
        if (p >= e || (*p != '0' && *p != '1')) return false;
        v = (*p == '1');
        ++p;
        return true;
    }
};

}  // namespace detail

inline bool parsePathData(const std::string& d, Path& out) {
    detail::Scanner sc{d.c_str(), d.c_str() + d.size()};
    double cx = 0, cy = 0, sx = 0, sy = 0;      // 当前点、子路径起点
    double lcx = 0, lcy = 0;                    // 上一个三次曲线的第二控制点(S 用)
    double lqx = 0, lqy = 0;                    // 上一个二次曲线的控制点(T 用)
    char prev = 0, cmd = 0;
    while (sc.more()) {
        if (sc.isCmd()) { cmd = *sc.p++; }
        else if (!cmd) return false;
        else if (cmd == 'M') cmd = 'L';        // 连续坐标:M 之后按 L,m 之后按 l
        else if (cmd == 'm') cmd = 'l';
        bool rel = cmd >= 'a' && cmd <= 'z';
        char C = (char)(rel ? cmd - 'a' + 'A' : cmd);
        double ox = rel ? cx : 0, oy = rel ? cy : 0;
        PathCmd pc;
        if (C == 'Z') {
            pc.op = PathCmd::Close;
            out.push_back(pc);
            cx = sx; cy = sy;
            prev = 'Z';
            continue;
        }
        if (C == 'M' || C == 'L') {
            double x, y;
            if (!sc.num(x) || !sc.num(y)) return false;
            pc.op = (C == 'M') ? PathCmd::Move : PathCmd::Line;
            pc.x = ox + x; pc.y = oy + y;
            cx = pc.x; cy = pc.y;
            if (C == 'M') { sx = cx; sy = cy; }
        } else if (C == 'H') {
            double x;
            if (!sc.num(x)) return false;
            pc.op = PathCmd::Line; pc.x = ox + x; pc.y = cy; cx = pc.x;
        } else if (C == 'V') {
            double y;
            if (!sc.num(y)) return false;
            pc.op = PathCmd::Line; pc.x = cx; pc.y = oy + y; cy = pc.y;
        } else if (C == 'C' || C == 'S') {
            double a, b, c2, d2, x, y;
            pc.op = PathCmd::Cubic;
            if (C == 'C') {
                if (!sc.num(a) || !sc.num(b)) return false;
                pc.x1 = ox + a; pc.y1 = oy + b;
            } else {
                bool wasCubic = (prev == 'C' || prev == 'S');
                pc.x1 = wasCubic ? 2 * cx - lcx : cx;
                pc.y1 = wasCubic ? 2 * cy - lcy : cy;
            }
            if (!sc.num(c2) || !sc.num(d2) || !sc.num(x) || !sc.num(y)) return false;
            pc.x2 = ox + c2; pc.y2 = oy + d2; pc.x = ox + x; pc.y = oy + y;
            lcx = pc.x2; lcy = pc.y2;
            cx = pc.x; cy = pc.y;
        } else if (C == 'Q' || C == 'T') {
            double qx, qy, x, y;
            if (C == 'Q') {
                if (!sc.num(qx) || !sc.num(qy)) return false;
                qx += ox; qy += oy;
            } else {
                bool wasQ = (prev == 'Q' || prev == 'T');
                qx = wasQ ? 2 * cx - lqx : cx;
                qy = wasQ ? 2 * cy - lqy : cy;
            }
            if (!sc.num(x) || !sc.num(y)) return false;
            x += ox; y += oy;
            pc.op = PathCmd::Cubic;  // 二次升成三次
            pc.x1 = cx + 2.0 / 3.0 * (qx - cx); pc.y1 = cy + 2.0 / 3.0 * (qy - cy);
            pc.x2 = x + 2.0 / 3.0 * (qx - x);   pc.y2 = y + 2.0 / 3.0 * (qy - y);
            pc.x = x; pc.y = y;
            lqx = qx; lqy = qy;
            cx = x; cy = y;
        } else if (C == 'A') {
            double rx, ry, rot, x, y;
            bool large, sweep;
            if (!sc.num(rx) || !sc.num(ry) || !sc.num(rot) || !sc.flag(large) || !sc.flag(sweep) || !sc.num(x) || !sc.num(y)) return false;
            pc.op = PathCmd::Arc;
            pc.rx = std::fabs(rx); pc.ry = std::fabs(ry); pc.rot = rot; pc.large = large; pc.sweep = sweep;
            pc.x = ox + x; pc.y = oy + y;
            cx = pc.x; cy = pc.y;
        } else {
            return false;
        }
        out.push_back(pc);
        prev = C;
    }
    return true;
}

// ---- 取属性
inline bool attrOf(const std::string& tag, const std::string& name, std::string& val) {
    size_t pos = 0;
    while ((pos = tag.find(name, pos)) != std::string::npos) {
        bool leftOk = pos > 0 && (tag[pos - 1] == ' ' || tag[pos - 1] == '\n' || tag[pos - 1] == '\t' || tag[pos - 1] == '\r');
        size_t q = pos + name.size();
        while (q < tag.size() && tag[q] == ' ') ++q;
        if (leftOk && q < tag.size() && tag[q] == '=') {
            ++q;
            while (q < tag.size() && tag[q] == ' ') ++q;
            if (q < tag.size() && (tag[q] == '"' || tag[q] == '\'')) {
                char quote = tag[q++];
                size_t e = tag.find(quote, q);
                if (e == std::string::npos) return false;
                val = tag.substr(q, e - q);
                return true;
            }
        }
        pos += name.size();
    }
    return false;
}

inline double numAttr(const std::string& tag, const std::string& name, double def) {
    std::string v;
    return attrOf(tag, name, v) ? std::strtod(v.c_str(), nullptr) : def;
}

inline void addEllipse(Path& p, double cx, double cy, double rx, double ry) {
    PathCmd m; m.op = PathCmd::Move; m.x = cx + rx; m.y = cy; p.push_back(m);
    PathCmd a; a.op = PathCmd::Arc; a.rx = rx; a.ry = ry; a.sweep = true;
    a.x = cx - rx; a.y = cy; p.push_back(a);
    a.x = cx + rx; a.y = cy; p.push_back(a);
    PathCmd c; c.op = PathCmd::Close; p.push_back(c);
}

inline void addRect(Path& p, double x, double y, double w, double h, double rx, double ry) {
    if (rx <= 0 && ry > 0) rx = ry;
    if (ry <= 0 && rx > 0) ry = rx;
    rx = std::fmin(rx, w / 2); ry = std::fmin(ry, h / 2);
    auto mv = [&](double a, double b) { PathCmd c; c.op = PathCmd::Move; c.x = a; c.y = b; p.push_back(c); };
    auto ln = [&](double a, double b) { PathCmd c; c.op = PathCmd::Line; c.x = a; c.y = b; p.push_back(c); };
    auto ar = [&](double a, double b) { PathCmd c; c.op = PathCmd::Arc; c.rx = rx; c.ry = ry; c.sweep = true; c.x = a; c.y = b; p.push_back(c); };
    mv(x + rx, y);
    ln(x + w - rx, y);
    if (rx > 0) ar(x + w, y + ry);
    ln(x + w, y + h - ry);
    if (rx > 0) ar(x + w - rx, y + h);
    ln(x + rx, y + h);
    if (rx > 0) ar(x, y + h - ry);
    ln(x, y + ry);
    if (rx > 0) ar(x + rx, y);
    PathCmd c; c.op = PathCmd::Close; p.push_back(c);
}

inline bool addPoints(Path& p, const std::string& pts, bool close) {
    detail::Scanner sc{pts.c_str(), pts.c_str() + pts.size()};
    bool first = true;
    double x, y;
    while (sc.num(x) && sc.num(y)) {
        PathCmd c;
        c.op = first ? PathCmd::Move : PathCmd::Line;
        c.x = x; c.y = y;
        p.push_back(c);
        first = false;
    }
    if (first) return false;
    if (close) { PathCmd c; c.op = PathCmd::Close; p.push_back(c); }
    return true;
}

inline bool parseSvg(const std::string& text, SvgIcon& icon) {
    icon = SvgIcon();
    size_t svgAt = text.find("<svg");
    if (svgAt == std::string::npos) return false;
    size_t svgEnd = text.find('>', svgAt);
    if (svgEnd == std::string::npos) return false;
    std::string head = text.substr(svgAt, svgEnd - svgAt);
    std::string v;
    if (attrOf(head, "viewBox", v)) {
        detail::Scanner sc{v.c_str(), v.c_str() + v.size()};
        double a, b, c, d;
        if (sc.num(a) && sc.num(b) && sc.num(c) && sc.num(d) && c > 0 && d > 0) { icon.vbX = a; icon.vbY = b; icon.vbW = c; icon.vbH = d; }
    }
    icon.strokeWidth = numAttr(head, "stroke-width", 2);
    if (attrOf(head, "fill", v)) icon.fill = (v != "none");
    if (attrOf(head, "stroke", v) && v.size() == 7 && v[0] == '#') {
        unsigned c = 0;
        bool good = true;
        for (size_t i = 1; i < 7; ++i) {
            char ch = v[i];
            c <<= 4;
            if (ch >= '0' && ch <= '9') c |= (unsigned)(ch - '0');
            else if (ch >= 'a' && ch <= 'f') c |= (unsigned)(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') c |= (unsigned)(ch - 'A' + 10);
            else { good = false; break; }
        }
        if (good) { icon.hasStroke = true; icon.stroke = c; }
    }
    size_t pos = svgEnd;
    while ((pos = text.find('<', pos)) != std::string::npos) {
        size_t e = text.find('>', pos);
        if (e == std::string::npos) break;
        std::string tag = text.substr(pos, e - pos);
        pos = e;
        if (tag.size() < 2 || tag[1] == '/' || tag[1] == '!' || tag[1] == '?') continue;
        size_t ne = 1;
        while (ne < tag.size() && tag[ne] != ' ' && tag[ne] != '\n' && tag[ne] != '\t' && tag[ne] != '\r' && tag[ne] != '/') ++ne;
        std::string name = tag.substr(1, ne - 1);
        Path p;
        bool ok = false;
        if (name == "path") {
            std::string d;
            ok = attrOf(tag, "d", d) && parsePathData(d, p);
        } else if (name == "circle") {
            double r = numAttr(tag, "r", 0);
            if (r > 0) { addEllipse(p, numAttr(tag, "cx", 0), numAttr(tag, "cy", 0), r, r); ok = true; }
        } else if (name == "ellipse") {
            double rx = numAttr(tag, "rx", 0), ry = numAttr(tag, "ry", 0);
            if (rx > 0 && ry > 0) { addEllipse(p, numAttr(tag, "cx", 0), numAttr(tag, "cy", 0), rx, ry); ok = true; }
        } else if (name == "line") {
            PathCmd m; m.op = PathCmd::Move; m.x = numAttr(tag, "x1", 0); m.y = numAttr(tag, "y1", 0); p.push_back(m);
            PathCmd l; l.op = PathCmd::Line; l.x = numAttr(tag, "x2", 0); l.y = numAttr(tag, "y2", 0); p.push_back(l);
            ok = true;
        } else if (name == "rect") {
            double w = numAttr(tag, "width", 0), h = numAttr(tag, "height", 0);
            if (w > 0 && h > 0) { addRect(p, numAttr(tag, "x", 0), numAttr(tag, "y", 0), w, h, numAttr(tag, "rx", 0), numAttr(tag, "ry", 0)); ok = true; }
        } else if (name == "polyline" || name == "polygon") {
            std::string pts;
            ok = attrOf(tag, "points", pts) && addPoints(p, pts, name == "polygon");
        }
        if (ok && !p.empty()) icon.paths.push_back(std::move(p));
    }
    return !icon.paths.empty();
}

}  // namespace xv
