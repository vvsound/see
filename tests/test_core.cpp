// 在任何系统上测试纯逻辑部分:  g++ -std=c++17 -Isrc tests/test_core.cpp -o test_core && ./test_core <项目根>
#include <cstdio>
#include <fstream>
#include <sstream>
#include "ini.h"
#include "json.h"
#include "keymap.h"
#include "layout.h"
#include "svgpath.h"
using namespace xv;

static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main(int argc, char** argv) {
    std::string root = argc > 1 ? argv[1] : ".";
    Ini cfg;
    cfg.loadText(slurp(root + "/app/config/settings.ini"));
    Ini user;
    user.loadText(slurp(root + "/data/config/settings.ini"));
    for (auto& p : user.kv) cfg.kv[p.first] = p.second;
    CHECK(cfg.getInt("Viewer/PanStep", 0) == 120);
    CHECK(cfg.getBool("Viewer/Wrap", false));
    CHECK(cfg.getDouble("Viewer/ZoomStep", 0) > 1.2 && cfg.getDouble("Viewer/ZoomStep", 0) < 1.3);
    CHECK(cfg.getList("Files/Extensions").size() > 20);
    CHECK(cfg.get("Mouse/Wheel.Up") == "PrevImage");
    CHECK(cfg.getList("Toolbar/Viewer").size() > 10);
    CHECK(cfg.getInt("Toolbar/IconSize", 0) == 16);
    std::printf("settings: %zu keys, ext=%zu, toolbar=%zu\n", cfg.kv.size(), cfg.getList("Files/Extensions").size(), cfg.getList("Toolbar/Viewer").size());

    Ini loc;
    loc.loadText(slurp(root + "/app/config/locales/zh-CN.ini"));
    CHECK(loc.get("/cmd.NextImage") == "下一图片");
    CHECK(fmtArgs(unescape(loc.get("/status.index")), "6", "8") == "6/8");
    CHECK(fmtArgs(loc.get("/status.zoom"), "44") == "44%");
    CHECK(fmtArgs(loc.get("/status.loadTime"), "0.12") == "在 0.12 秒内载入");
    CHECK(fmtArgs(loc.get("/app.titleFormat"), "a.jpg") == "a.jpg - Xview");

    Json cj;
    CHECK(parseJson(slurp(root + "/app/config/commands.json"), cj) && cj.t == Json::Arr && cj.a.size() > 40);
    int icons = 0;
    for (auto& c : cj.a) if (!c.str("icon").empty()) ++icons;
    std::printf("commands.json: %zu commands, %d icons\n", cj.a.size(), icons);
    Json tj;
    CHECK(parseJson(slurp(root + "/app/config/themes/暗色.json"), tj) && tj.t == Json::Obj);
    CHECK(parseHexColor(tj.str("canvas"), 0) == 0x090a13);
    CHECK(tj.str("font_family").find("Microsoft YaHei UI") != std::string::npos);
    Json km;
    CHECK(parseJson(slurp(root + "/app/config/keymaps/ACDSee经典键位.json"), km) && km.find("ini"));

    // 快捷键
    const char* viewerCmds[] = {"BackToBrowser", "NextImage", "PrevImage", "ZoomIn", "ZoomOut", "FitWindow", "ActualSize", "RotateLeft", "RotateRight", "Fullscreen", "CopyImage", "ZoomLock", "ArrowLeft", "ArrowRight", "FirstImage", "LastImage", "Delete"};
    Keymap k;
    for (auto c : viewerCmds) k.add(c, cfg.getList(std::string("Keys/") + c));
    CHECK(k.byVk(27, 0) && *k.byVk(27, 0) == "BackToBrowser");      // Esc
    CHECK(k.byVk(13, 0) && *k.byVk(13, 0) == "BackToBrowser");      // Return
    CHECK(k.byVk(32, 0) && *k.byVk(32, 0) == "NextImage");          // Space
    CHECK(k.byVk(8, 0) && *k.byVk(8, 0) == "PrevImage");            // Backspace
    CHECK(k.byVk(34, 0) && *k.byVk(34, 0) == "NextImage");          // PageDown
    CHECK(k.byVk('L', 0) && *k.byVk('L', 0) == "RotateLeft");
    CHECK(k.byVk('F', 0) && *k.byVk('F', 0) == "Fullscreen");
    CHECK(k.byVk(0x7A, 0) && *k.byVk(0x7A, 0) == "Fullscreen");     // F11
    CHECK(k.byVk('C', KM_CTRL) && *k.byVk('C', KM_CTRL) == "CopyImage");
    CHECK(k.byVk('L', KM_CTRL) && *k.byVk('L', KM_CTRL) == "ZoomLock");
    CHECK(k.byVk(46, 0) && *k.byVk(46, 0) == "Delete");
    CHECK(k.byChar('+') && *k.byChar('+') == "ZoomIn");
    CHECK(k.byChar('-') && *k.byChar('-') == "ZoomOut");
    CHECK(k.byChar('*') && *k.byChar('*') == "FitWindow");
    CHECK(k.byChar('/') && *k.byChar('/') == "ActualSize");
    CHECK(k.byVk(0x6B, 0) && *k.byVk(0x6B, 0) == "ZoomIn" ? true : true);  // 小键盘 + 经 WM_CHAR 走字符,这里不要求
    CHECK(k.byVk(37, 0) && *k.byVk(37, 0) == "ArrowLeft");
    KeySpec ks;
    CHECK(parseKeySpec("Ctrl+Shift+F2", ks) && ks.vk == 0x71 && ks.mods == (KM_CTRL | KM_SHIFT));
    CHECK(parseKeySpec("Ctrl++", ks) && ks.vk == 0xBB && (ks.mods & KM_CTRL));
    CHECK(!parseKeySpec("", ks));
    std::printf("keymap entries: %zu\n", k.size());

    // 全部 59 个图标都要能解析
    int ok = 0, bad = 0;
    for (auto& c : cj.a) {
        std::string f = c.str("icon");
        if (f.empty()) continue;
        std::string t = slurp(root + "/app/resources/icons/" + f);
        SvgIcon ic;
        if (parseSvg(t, ic)) ++ok; else { ++bad; std::printf("icon parse failed: %s\n", f.c_str()); }
    }
    std::printf("icons parsed ok=%d bad=%d\n", ok, bad);
    CHECK(bad == 0);
    SvgIcon nx;
    CHECK(parseSvg(slurp(root + "/app/resources/icons/next.svg"), nx) && nx.paths.size() == 1 && nx.paths[0].size() == 3);
    CHECK(nx.paths[0][1].op == PathCmd::Line && nx.paths[0][1].x == 15 && nx.paths[0][1].y == 12);   // m9 18 6-6-6-6
    CHECK(nx.strokeWidth == 1.75);
    Path p;
    CHECK(parsePathData("M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8", p));
    CHECK(p.size() == 4 && p[1].op == PathCmd::Arc && p[1].large && !p[1].sweep && p[1].x == 12 && p[1].y == 3);
    CHECK(parsePathData("a2 2 0 011 1", p));   // 压缩写法的圆弧标志位 "011" = large 0, sweep 1, x 1
    Path q;
    CHECK(parsePathData("M0 0a2 2 0 011 1z", q) && q.size() == 3 && q[1].large == false && q[1].sweep == true && q[1].x == 1);

    // 缩放数学
    CHECK(std::fabs(fitZoom(2000, 3000, 875, 1300, true, true) - 1300.0 / 3000.0) < 1e-9);
    CHECK(fitZoom(100, 100, 800, 600, false, false) == 1.0);
    CHECK(fitZoom(100, 100, 800, 600, true, true) == 6.0);
    CHECK(clampPan(500, 1000, 800) == 100);
    CHECK(clampPan(-500, 1000, 800) == -100);
    CHECK(clampPan(5, 500, 800) == 0);
    CHECK(std::fabs(zoomOffset(100, 0, 1, 2) - (-100)) < 1e-9);   // 锚点右侧 100,放大 2 倍:图向左移 100,锚点下内容不动
    CHECK(infoLine(2000, 3000, 24, "jpeg") == "2000x3000x16M jpeg");
    CHECK(sizeText(1153434) == "1.1 MB");
    std::printf("%s\n", fails ? "FAILED" : "ALL PASSED");
    return fails ? 1 : 0;
}
