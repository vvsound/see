// Xview 原生版:纯 C++ / Win32 / Direct2D / WIC 看图程序。
//  - 解码:WIC(系统自带,JPG PNG BMP GIF TIFF ICO,以及系统装了编码器的 WebP HEIF AVIF 等)
//  - 显示:Direct2D(GPU 贴图,缩放平移不重新解码)
//  - 配置:全部读外置文件(app\config\*、data\config\*),本程序只写 data\config\native.ini
//  - 读不了的格式(RAW PSD SVG HDR…)和 "--" 开头的命令行选项:交给 Python 版(app\src\main.pyw)
// 目录布局和 Python 版一致:Xview.exe 放在项目根,旁边是 app\ 和 data\。
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "ini.h"
#include "json.h"
#include "keymap.h"
#include "layout.h"
#include "svgpath.h"

using Microsoft::WRL::ComPtr;
using namespace xv;

namespace {

constexpr wchar_t kClass[] = L"XviewNativeWnd";
constexpr UINT WM_APP_LOADED = WM_APP + 1;
constexpr UINT_PTR TIMER_FLIP = 1;
constexpr UINT_PTR TIMER_CURSOR = 2;
constexpr ULONG_PTR kCopyDataPath = 0x58564E31;  // 'XVN1'

// ------------------------------------------------------------------ 字符串 / 文件小工具
std::wstring W(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string U(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring lowerW(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

bool fileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string readFileText(const std::wstring& path) {
    std::string out;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return out;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

bool writeFileText(const std::wstring& path, const std::string& data) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

std::wstring parentDir(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"" : p.substr(0, i);
}

std::wstring baseName(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

std::wstring extOf(const std::wstring& p) {
    std::wstring n = baseName(p);
    size_t i = n.find_last_of(L'.');
    return i == std::wstring::npos ? L"" : lowerW(n.substr(i + 1));
}

std::wstring absPath(const std::wstring& p) {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 4, buf, nullptr);
    return (n > 0 && n < MAX_PATH * 4) ? std::wstring(buf, n) : p;
}

std::wstring exeDir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    return parentDir(std::wstring(buf, n));
}

std::wstring quoteArg(const std::wstring& s) {
    std::wstring o = L"\"";
    size_t bs = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { ++bs; continue; }
        if (c == L'"') { o.append(bs * 2 + 1, L'\\'); o += L'"'; }
        else { o.append(bs, L'\\'); o += c; }
        bs = 0;
    }
    o.append(bs * 2, L'\\');
    return o + L"\"";
}

D2D1_COLOR_F colorOf(unsigned rgb, float a = 1.0f) {
    return D2D1::ColorF(rgb, a);
}

// ------------------------------------------------------------------ 解码
struct Loaded {
    std::wstring path;
    bool ok = false;
    std::string err;
    ComPtr<IWICBitmap> bmp;
    UINT origW = 0, origH = 0;   // 摆正后的原图尺寸
    UINT texW = 0, texH = 0;     // 实际像素尺寸(超过显卡上限时会缩小)
    int bits = 0;
    std::string format;
    unsigned long long fileSize = 0;
    double secs = 0;
    size_t bytes = 0;
};

ComPtr<IWICImagingFactory> g_wic;

bool queryOrientation(IWICBitmapFrameDecode* frame, int& o) {
    ComPtr<IWICMetadataQueryReader> mq;
    if (FAILED(frame->GetMetadataQueryReader(&mq)) || !mq) return false;
    const wchar_t* queries[] = {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}", L"/xmp/tiff:Orientation"};
    for (auto q : queries) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(mq->GetMetadataByName(q, &pv))) {
            int v = 0;
            if (pv.vt == VT_UI2) v = pv.uiVal;
            else if (pv.vt == VT_UI1) v = pv.bVal;
            else if (pv.vt == VT_UI4) v = (int)pv.ulVal;
            PropVariantClear(&pv);
            if (v >= 1 && v <= 8) { o = v; return true; }
        } else {
            PropVariantClear(&pv);
        }
    }
    return false;
}

bool scaleDownTo(IWICBitmap* in, UINT maxSide, ComPtr<IWICBitmap>& out, UINT& w, UINT& h) {
    UINT iw = 0, ih = 0;
    in->GetSize(&iw, &ih);
    if (iw <= maxSide && ih <= maxSide) { out = in; w = iw; h = ih; return true; }
    double k = (double)maxSide / (double)std::max(iw, ih);
    UINT nw = std::max<UINT>(1, (UINT)(iw * k)), nh = std::max<UINT>(1, (UINT)(ih * k));
    ComPtr<IWICBitmapScaler> sc;
    if (FAILED(g_wic->CreateBitmapScaler(&sc))) return false;
    if (FAILED(sc->Initialize(in, nw, nh, WICBitmapInterpolationModeFant))) return false;
    ComPtr<IWICBitmap> nb;
    if (FAILED(g_wic->CreateBitmapFromSource(sc.Get(), WICBitmapCacheOnLoad, &nb))) return false;
    out = nb; w = nw; h = nh;
    return true;
}

std::shared_ptr<Loaded> decodeFile(const std::wstring& path) {
    auto t0 = std::chrono::steady_clock::now();
    auto r = std::make_shared<Loaded>();
    r->path = path;
    r->err = "无法读取这张图片(文件损坏,或者系统没有这种格式的解码器)";
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        r->fileSize = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    std::wstring ext = extOf(path);
    r->format = (ext == L"jpg" || ext == L"jpe" || ext == L"jfif") ? "jpeg" : U(ext);

    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(g_wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return r;
    GUID cf;
    if (SUCCEEDED(dec->GetContainerFormat(&cf))) {
        if (cf == GUID_ContainerFormatJpeg) r->format = "jpeg";
        else if (cf == GUID_ContainerFormatPng) r->format = "png";
        else if (cf == GUID_ContainerFormatGif) r->format = "gif";
        else if (cf == GUID_ContainerFormatBmp) r->format = "bmp";
        else if (cf == GUID_ContainerFormatTiff) r->format = "tiff";
        else if (cf == GUID_ContainerFormatIco) r->format = "ico";
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return r;
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) return r;

    WICPixelFormatGUID pf;
    if (SUCCEEDED(frame->GetPixelFormat(&pf))) {
        ComPtr<IWICComponentInfo> ci;
        if (SUCCEEDED(g_wic->CreateComponentInfo(pf, &ci))) {
            ComPtr<IWICPixelFormatInfo> pi;
            if (SUCCEEDED(ci.As(&pi))) {
                UINT bpp = 0;
                if (SUCCEEDED(pi->GetBitsPerPixel(&bpp))) r->bits = (int)bpp;
            }
        }
    }
    int orient = 1;
    queryOrientation(frame.Get(), orient);

    // 先缩到不超过 16384(再大的图显卡贴不上),再转成预乘 BGRA
    ComPtr<IWICBitmapSource> src;
    frame.As(&src);
    const UINT kMaxSide = 16384;
    if (w > kMaxSide || h > kMaxSide) {
        double k = (double)kMaxSide / (double)std::max(w, h);
        UINT nw = std::max<UINT>(1, (UINT)(w * k)), nh = std::max<UINT>(1, (UINT)(h * k));
        ComPtr<IWICBitmapScaler> sc;
        if (FAILED(g_wic->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(src.Get(), nw, nh, WICBitmapInterpolationModeFant))) return r;
        sc.As(&src);
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(g_wic->CreateFormatConverter(&conv))) return r;
    if (FAILED(conv->Initialize(src.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut))) return r;
    ComPtr<IWICBitmapSource> cur;
    conv.As(&cur);

    auto xform = [&](WICBitmapTransformOptions o) {
        ComPtr<IWICBitmapFlipRotator> fr;
        if (SUCCEEDED(g_wic->CreateBitmapFlipRotator(&fr)) && SUCCEEDED(fr->Initialize(cur.Get(), o))) fr.As(&cur);
    };
    switch (orient) {  // EXIF 方向:摆正
        case 2: xform(WICBitmapTransformFlipHorizontal); break;
        case 3: xform(WICBitmapTransformRotate180); break;
        case 4: xform(WICBitmapTransformFlipVertical); break;
        case 5: xform(WICBitmapTransformRotate90); xform(WICBitmapTransformFlipHorizontal); break;
        case 6: xform(WICBitmapTransformRotate90); break;
        case 7: xform(WICBitmapTransformRotate270); xform(WICBitmapTransformFlipHorizontal); break;
        case 8: xform(WICBitmapTransformRotate270); break;
        default: break;
    }
    ComPtr<IWICBitmap> bmp;
    if (FAILED(g_wic->CreateBitmapFromSource(cur.Get(), WICBitmapCacheOnLoad, &bmp))) return r;
    UINT tw = 0, th = 0;
    bmp->GetSize(&tw, &th);
    r->bmp = bmp;
    r->texW = tw;
    r->texH = th;
    // 原图尺寸:被缩过就按比例还原(转 90 度时宽高对调)
    bool swap = orient >= 5;
    UINT ow = swap ? h : w, oh = swap ? w : h;
    r->origW = ow;
    r->origH = oh;
    r->bytes = (size_t)tw * th * 4;
    r->ok = true;
    r->err.clear();
    r->secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

// ------------------------------------------------------------------ 缓存(预读也放这里)
class Cache {
public:
    explicit Cache(size_t limitBytes) : limit_(limitBytes) {}
    std::shared_ptr<Loaded> get(const std::wstring& path) {
        std::lock_guard<std::mutex> g(m_);
        auto it = map_.find(lowerW(path));
        if (it == map_.end()) return nullptr;
        touch(it->first);
        return it->second;
    }
    void put(const std::wstring& path, std::shared_ptr<Loaded> v) {
        std::lock_guard<std::mutex> g(m_);
        std::wstring k = lowerW(path);
        auto it = map_.find(k);
        if (it != map_.end()) { bytes_ -= it->second->bytes; lru_.remove(k); map_.erase(it); }
        map_[k] = v;
        bytes_ += v->bytes;
        lru_.push_front(k);
        while (bytes_ > limit_ && lru_.size() > 2) {   // 至少留 2 张(当前 + 下一张)
            std::wstring old = lru_.back();
            lru_.pop_back();
            auto o = map_.find(old);
            if (o != map_.end()) { bytes_ -= o->second->bytes; map_.erase(o); }
        }
    }
    bool beginLoad(const std::wstring& path) {   // true = 调用者去解码;false = 已缓存或别人正在解
        std::lock_guard<std::mutex> g(m_);
        std::wstring k = lowerW(path);
        if (map_.count(k) || inflight_.count(k)) return false;
        inflight_.insert(k);
        return true;
    }
    void endLoad(const std::wstring& path) {
        std::lock_guard<std::mutex> g(m_);
        inflight_.erase(lowerW(path));
    }
    bool busy(const std::wstring& path) {
        std::lock_guard<std::mutex> g(m_);
        return inflight_.count(lowerW(path)) != 0;
    }
    void erase(const std::wstring& path) {
        std::lock_guard<std::mutex> g(m_);
        std::wstring k = lowerW(path);
        auto it = map_.find(k);
        if (it != map_.end()) { bytes_ -= it->second->bytes; map_.erase(it); lru_.remove(k); }
    }
    void clear() {
        std::lock_guard<std::mutex> g(m_);
        map_.clear();
        lru_.clear();
        bytes_ = 0;
    }

private:
    void touch(const std::wstring& k) { lru_.remove(k); lru_.push_front(k); }
    std::mutex m_;
    std::map<std::wstring, std::shared_ptr<Loaded>> map_;
    std::list<std::wstring> lru_;
    std::set<std::wstring> inflight_;
    size_t bytes_ = 0, limit_;
};

// ------------------------------------------------------------------ Python 版(备用)
std::wstring firstLineOf(const std::wstring& file) {
    std::string s = readFileText(file);
    if (s.compare(0, 3, "\xEF\xBB\xBF") == 0) s.erase(0, 3);
    size_t e = s.find_first_of("\r\n");
    if (e != std::string::npos) s.erase(e);
    std::wstring w = W(s);
    while (!w.empty() && (w.back() == L' ' || w.back() == L'\t' || w.back() == L'"')) w.pop_back();
    size_t b = 0;
    while (b < w.size() && (w[b] == L' ' || w[b] == L'\t' || w[b] == L'"')) ++b;
    return w.substr(b);
}

std::wstring findPythonw(const std::wstring& root) {
    std::wstring cfg = firstLineOf(root + L"\\data\\config\\python.txt");
    if (!cfg.empty()) {
        bool abs = (cfg.size() >= 2 && cfg[1] == L':') || cfg.rfind(L"\\\\", 0) == 0;
        std::wstring p = abs ? cfg : root + L"\\" + cfg;
        if (dirExists(p)) p += L"\\pythonw.exe";
        if (fileExists(p)) return p;
    }
    wchar_t buf[MAX_PATH * 4];
    DWORD n = SearchPathW(nullptr, L"pythonw.exe", nullptr, MAX_PATH * 4, buf, nullptr);
    return (n > 0 && n < MAX_PATH * 4) ? std::wstring(buf, n) : L"";
}

bool pythonAvailable(const std::wstring& root) {
    return fileExists(root + L"\\app\\src\\main.pyw") && !findPythonw(root).empty();
}

bool launchPython(const std::wstring& root, const std::vector<std::wstring>& args) {
    std::wstring py = findPythonw(root);
    std::wstring script = root + L"\\app\\src\\main.pyw";
    if (py.empty() || !fileExists(script)) return false;
    std::wstring cmd = quoteArg(py) + L" " + quoteArg(script);
    for (auto& a : args) cmd += L" " + quoteArg(a);
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(py.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, root.c_str(), &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// ------------------------------------------------------------------ 解码线程池(预读 + 当前图)
class Pool {
public:
    void start(int n, void (*fn)(const std::wstring&)) {
        fn_ = fn;
        for (int i = 0; i < n; ++i) std::thread([this] { loop(); }).detach();   // 进程退出时一起结束
    }
    void push(const std::wstring& p, bool front) {
        {
            std::lock_guard<std::mutex> g(m_);
            if (front) q_.push_front(p); else q_.push_back(p);
        }
        cv_.notify_one();
    }

private:
    void loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // 不 CoUninitialize:线程跟着进程活到底
        for (;;) {
            std::wstring p;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return !q_.empty(); });
                p = q_.front();
                q_.pop_front();
            }
            fn_(p);
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::wstring> q_;
    void (*fn_)(const std::wstring&) = nullptr;
};

// ------------------------------------------------------------------ 主题
struct Theme {
    unsigned toolbarBg = 0xE8EEF4, statusBg = 0xDCE6EE, statusFg = 0x303030, icon = 0x3D3D3D;
    unsigned canvas = 0x202020, canvasErr = 0xFFA0A0, hover = 0xC8D8E8, border = 0xA0B0C0, accentSoft = 0x9CB8D8;
    std::wstring font = L"Microsoft YaHei UI";
    float fontPt = 9.0f;
    int toolbarH = 26, statusH = 18;
};

int pxOf(const std::string& s, int def) {
    if (s.empty()) return def;
    int v = std::atoi(s.c_str());
    return v > 0 ? v : def;
}

// 命令:原生版实现了的查看窗口命令(顺序 = 快捷键冲突时的优先级)
const char* const kCmds[] = {
    "BackToBrowser", "CloseImage", "Exit", "QuitBrowser",
    "NextImage", "PrevImage", "FirstImage", "LastImage",
    "ZoomIn", "ZoomOut", "FitWindow", "ActualSize", "ZoomLock",
    "RotateLeft", "RotateRight", "Fullscreen", "AlwaysOnTop",
    "ReloadImage", "OpenFile", "CopyImage", "CopyPath", "ShowInExplorer", "Properties", "Delete",
    "ToggleToolbar", "ToggleStatusBar",
    "ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown"};

bool implemented(const std::string& c) {
    for (auto k : kCmds) if (c == k) return true;
    return false;
}

struct Icon {
    SvgIcon svg;
    std::vector<ComPtr<ID2D1PathGeometry>> geos;
    bool ok = false;
};

struct Button {
    std::string cmd;
    bool sep = false;
    RECT r{};
};

// ------------------------------------------------------------------ 应用
struct App {
    HWND hwnd = nullptr;
    std::wstring root;
    Ini cfg, loc, nat;
    Theme th;
    std::map<std::string, std::string> iconOf;
    Keymap keys;
    std::set<std::wstring> exts;
    Cache cache{512ull << 20};
    Pool pool;
    int dpi = 96;

    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1Bitmap> bitmap;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IDWriteTextFormat> tf;
    ComPtr<ID2D1StrokeStyle> stroke;
    std::map<std::string, Icon> icons;

    std::vector<std::wstring> files;
    int idx = -1;
    std::shared_ptr<Loaded> cur;
    std::string runtimeErr;

    double zoom = 1.0, offX = 0, offY = 0;
    int rot = 0;
    bool fitAuto = true, zoomLock = false;
    bool fullscreen = false, topmost = false, showToolbar = true, showStatus = true;
    bool anchorValid = false;
    POINT anchor{0, 0};
    bool dragging = false;
    POINT dragLast{0, 0};
    bool handledKey = false;
    DWORD lastGo = 0, lastMouse = 0;
    bool cursorHidden = false;
    LONG_PTR prevStyle = 0;
    WINDOWPLACEMENT prevPlace{};

    std::vector<Button> buttons;
    int hotBtn = -1, downBtn = -1;
    bool trackingLeave = false;

    // ---------------- 小工具
    int S(int v) const { return MulDiv(v, dpi, 96); }
    std::string tr(const std::string& key, const std::string& def = "") const { return unescape(loc.get("/" + key, def)); }
    std::string cmdLabel(const std::string& id) const { return tr("cmd." + id, id); }
    int tbH() const { return (showToolbar && !fullscreen) ? S(th.toolbarH) : 0; }
    int stH() const { return (showStatus && !fullscreen) ? S(th.statusH) : 0; }
    RECT client() const { RECT r; GetClientRect(hwnd, &r); return r; }
    RECT canvasRect() const {
        RECT r = client();
        r.top = tbH();
        r.bottom = std::max(r.top, r.bottom - stH());
        return r;
    }
    bool hasImage() const { return cur && cur->ok && bitmap; }
    double effW() const { return (rot & 1) ? cur->origH : cur->origW; }
    double effH() const { return (rot & 1) ? cur->origW : cur->origH; }
    void invalidate() { if (hwnd) InvalidateRect(hwnd, nullptr, FALSE); }

    // ---------------- 配置加载
    void loadConfig() {
        Ini tpl;
        tpl.loadText(readFileText(root + L"\\app\\config\\settings.ini"));
        cfg = tpl;
        Ini user;
        user.loadText(readFileText(root + L"\\data\\config\\settings.ini"));
        for (auto& p : user.kv) cfg.kv[p.first] = p.second;
        nat.loadText(readFileText(root + L"\\data\\config\\native.ini"));

        std::string lang = trim(readFileText(root + L"\\data\\config\\locale.txt"));
        if (lang.compare(0, 3, "\xEF\xBB\xBF") == 0) lang.erase(0, 3);
        if (lang.empty()) lang = "zh-CN";
        loc.loadText(readFileText(root + L"\\app\\config\\locales\\" + W(lang) + L".ini"));

        // 命令表 → 图标文件名
        Json cj;
        if (parseJson(readFileText(root + L"\\app\\config\\commands.json"), cj) && cj.t == Json::Arr)
            for (auto& c : cj.a) iconOf[c.str("id")] = c.str("icon");

        // 主题
        std::string tname = trim(readFileText(root + L"\\data\\config\\theme.txt"));
        if (tname.compare(0, 3, "\xEF\xBB\xBF") == 0) tname.erase(0, 3);
        if (!tname.empty()) {
            Json tj;
            if (parseJson(readFileText(root + L"\\app\\config\\themes\\" + W(tname) + L".json"), tj) && tj.t == Json::Obj) {
                auto col = [&](const char* k, unsigned& dst) { dst = parseHexColor(tj.str(k), dst); };
                col("toolbar_bg", th.toolbarBg); col("status_bg", th.statusBg); col("status_fg", th.statusFg);
                col("icon", th.icon); col("canvas", th.canvas); col("canvas_error", th.canvasErr);
                col("hover", th.hover); col("border", th.border); col("accent_soft", th.accentSoft);
                if (!tj.find("canvas")) th.canvas = parseHexColor(tj.str("bg"), th.canvas);
                if (!tj.find("canvas_error")) th.canvasErr = parseHexColor(tj.str("danger"), th.canvasErr);
                th.toolbarH = pxOf(tj.str("toolbar_h"), th.toolbarH);
                th.statusH = pxOf(tj.str("status_h"), th.statusH);
                std::string fam = tj.str("font_family");   // "\"Microsoft YaHei UI\", \"Segoe UI\"" 取第一个
                size_t a = fam.find('"');
                if (a != std::string::npos) { size_t b = fam.find('"', a + 1); if (b != std::string::npos) th.font = W(fam.substr(a + 1, b - a - 1)); }
                else if (!fam.empty()) th.font = W(trim(fam.substr(0, fam.find(','))));
                float pt = (float)std::atof(tj.str("font_size").c_str());
                if (pt > 4 && pt < 40) th.fontPt = pt;
            }
        }
        // 图片扩展名
        for (auto& e : cfg.getList("Files/Extensions")) exts.insert(lowerW(W(e)));
        if (exts.empty()) for (auto e : {L"jpg", L"jpeg", L"png", L"gif", L"bmp", L"webp", L"tif", L"tiff", L"ico", L"jfif"}) exts.insert(e);
        // 快捷键:只收录原生版实现了的命令
        for (auto c : kCmds) keys.add(c, cfg.getList(std::string("Keys/") + c));
        showToolbar = cfg.getBool("Viewer/ShowToolbar", true);
        showStatus = cfg.getBool("Viewer/ShowStatusBar", true);
        zoomLock = cfg.getBool("Viewer/ZoomLock", false);
        topmost = cfg.getBool("Viewer/AlwaysOnTop", false);
    }

    // ---------------- Direct2D / DirectWrite
    bool initGraphics() {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&d2d)))) return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf())))) return false;
        d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_SOLID, 0.0f), nullptr, 0, &stroke);
        makeTextFormat();
        return true;
    }
    void makeTextFormat() {
        tf.Reset();
        float px = th.fontPt * (float)dpi / 72.0f;
        if (SUCCEEDED(dw->CreateTextFormat(th.font.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"zh-cn", &tf))) {
            tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }
    bool ensureTarget() {
        if (rt) return true;
        RECT rc = client();
        D2D1_SIZE_U sz = D2D1::SizeU((UINT32)std::max<LONG>(1, rc.right), (UINT32)std::max<LONG>(1, rc.bottom));
        HRESULT hr = d2d->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), 96.0f, 96.0f),
            D2D1::HwndRenderTargetProperties(hwnd, sz), &rt);
        if (FAILED(hr)) return false;
        rt->CreateSolidColorBrush(D2D1::ColorF(0), &brush);
        return true;
    }
    void dropTarget() {
        runtimeErr.clear();
        bitmap.Reset();
        brush.Reset();
        rt.Reset();
    }
    bool ensureBitmap() {
        if (bitmap) return true;
        if (!cur || !cur->ok || !rt || !runtimeErr.empty()) return false;
        HRESULT hr = rt->CreateBitmapFromWicBitmap(cur->bmp.Get(), &bitmap);
        if (FAILED(hr)) {   // 显卡贴图上限比图小:缩小后再贴(显示尺寸仍按原图算)
            ComPtr<IWICBitmap> nb;
            UINT nw, nh;
            if (scaleDownTo(cur->bmp.Get(), rt->GetMaximumBitmapSize(), nb, nw, nh)) hr = rt->CreateBitmapFromWicBitmap(nb.Get(), &bitmap);
        }
        if (FAILED(hr)) runtimeErr = "这张图太大,显卡贴不上";
        return bitmap != nullptr;
    }

    // ---------------- 图标
    Icon* iconFor(const std::string& cmd) {
        auto fi = iconOf.find(cmd);
        if (fi == iconOf.end() || fi->second.empty()) return nullptr;
        const std::string& file = fi->second;
        auto it = icons.find(file);
        if (it != icons.end()) return it->second.ok ? &it->second : nullptr;
        Icon& ic = icons[file];
        std::string txt = readFileText(root + L"\\data\\config\\icons\\" + W(file));
        if (txt.empty()) txt = readFileText(root + L"\\app\\resources\\icons\\" + W(file));
        if (!txt.empty() && parseSvg(txt, ic.svg)) {
            for (auto& p : ic.svg.paths) {
                ComPtr<ID2D1PathGeometry> g = buildGeometry(p);
                if (g) ic.geos.push_back(g);
            }
            ic.ok = !ic.geos.empty();
        }
        return ic.ok ? &ic : nullptr;
    }
    ComPtr<ID2D1PathGeometry> buildGeometry(const Path& p) {
        ComPtr<ID2D1PathGeometry> geo;
        if (FAILED(d2d->CreatePathGeometry(&geo))) return nullptr;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(geo->Open(&sink))) return nullptr;
        bool open = false;
        double cx = 0, cy = 0, sx = 0, sy = 0;
        auto begin = [&](double x, double y) {
            sink->BeginFigure(D2D1::Point2F((float)x, (float)y), D2D1_FIGURE_BEGIN_HOLLOW);
            open = true; sx = x; sy = y;
        };
        for (auto& c : p) {
            switch (c.op) {
                case PathCmd::Move:
                    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    begin(c.x, c.y); cx = c.x; cy = c.y;
                    break;
                case PathCmd::Line:
                    if (!open) begin(cx, cy);
                    sink->AddLine(D2D1::Point2F((float)c.x, (float)c.y)); cx = c.x; cy = c.y;
                    break;
                case PathCmd::Cubic:
                    if (!open) begin(cx, cy);
                    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F((float)c.x1, (float)c.y1), D2D1::Point2F((float)c.x2, (float)c.y2), D2D1::Point2F((float)c.x, (float)c.y)));
                    cx = c.x; cy = c.y;
                    break;
                case PathCmd::Arc:
                    if (!open) begin(cx, cy);
                    if (c.rx <= 0 || c.ry <= 0) sink->AddLine(D2D1::Point2F((float)c.x, (float)c.y));
                    else sink->AddArc(D2D1::ArcSegment(D2D1::Point2F((float)c.x, (float)c.y), D2D1::SizeF((float)c.rx, (float)c.ry), (float)c.rot,
                                                       c.sweep ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                                       c.large ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
                    cx = c.x; cy = c.y;
                    break;
                case PathCmd::Close:
                    if (open) { sink->EndFigure(D2D1_FIGURE_END_CLOSED); open = false; cx = sx; cy = sy; }
                    break;
            }
        }
        if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
        if (FAILED(sink->Close())) return nullptr;
        return geo;
    }

    // ---------------- 工具栏
    bool checked(const std::string& c) const {
        if (c == "Fullscreen") return fullscreen;
        if (c == "AlwaysOnTop") return topmost;
        if (c == "ZoomLock") return zoomLock;
        if (c == "ToggleToolbar") return showToolbar;
        if (c == "ToggleStatusBar") return showStatus;
        return false;
    }
    void layoutToolbar() {
        buttons.clear();
        if (!tbH()) return;
        int iconPx = S(std::max(12, std::min(64, cfg.getInt("Toolbar/IconSize", 16))));
        int bw = iconPx + S(10), x = S(4), h = tbH();
        bool lastSep = true;
        for (auto& tok : cfg.getList("Toolbar/Viewer")) {
            Button b;
            if (tok == "|") {
                if (lastSep) continue;
                b.sep = true;
                b.r = {x, S(4), x + S(8), h - S(4)};
                x += S(8);
                lastSep = true;
            } else {
                if (tok[0] == '@' || !implemented(tok)) continue;
                b.cmd = tok;
                b.r = {x, S(1), x + bw, h - S(1)};
                x += bw + S(1);
                lastSep = false;
            }
            buttons.push_back(b);
        }
        if (!buttons.empty() && buttons.back().sep) buttons.pop_back();
    }
    int buttonAt(int px, int py) const {
        for (size_t i = 0; i < buttons.size(); ++i) {
            const RECT& r = buttons[i].r;
            if (!buttons[i].sep && px >= r.left && px < r.right && py >= r.top && py < r.bottom) return (int)i;
        }
        return -1;
    }
    std::string hintFor(const std::string& cmd) const {
        std::string s = cmdLabel(cmd);
        auto k = cfg.getList("Keys/" + cmd);
        if (!k.empty()) s += "  (" + k[0] + ")";
        return s;
    }

    // ---------------- 绘制
    void setBrush(unsigned rgb, float a = 1.0f) { brush->SetColor(colorOf(rgb, a)); }
    void fillRect(const RECT& r, unsigned rgb) {
        setBrush(rgb);
        rt->FillRectangle(D2D1::RectF((float)r.left, (float)r.top, (float)r.right, (float)r.bottom), brush.Get());
    }
    void drawText(const std::string& s, const RECT& r, unsigned rgb, DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (!tf || s.empty()) return;
        std::wstring w = W(s);
        tf->SetTextAlignment(al);
        setBrush(rgb);
        rt->DrawText(w.c_str(), (UINT32)w.size(), tf.Get(), D2D1::RectF((float)r.left, (float)r.top, (float)r.right, (float)r.bottom), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void drawIcon(Icon& ic, float x, float y, float size) {
        float s = size / (float)ic.svg.vbW;
        rt->SetTransform(D2D1::Matrix3x2F::Translation((float)-ic.svg.vbX, (float)-ic.svg.vbY) * D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(x, y));
        setBrush(th.icon);
        for (auto& g : ic.geos) rt->DrawGeometry(g.Get(), brush.Get(), (float)ic.svg.strokeWidth, stroke.Get());
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
    }
    void drawImage() {
        RECT c = canvasRect();
        float vw = (float)(c.right - c.left), vh = (float)(c.bottom - c.top);
        float cx = c.left + vw / 2 + (float)offX, cy = c.top + vh / 2 + (float)offY;
        float dw = (float)(cur->origW * zoom), dh = (float)(cur->origH * zoom);
        rt->PushAxisAlignedClip(D2D1::RectF((float)c.left, (float)c.top, (float)c.right, (float)c.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        rt->SetTransform(D2D1::Matrix3x2F::Rotation((float)rot * 90.0f, D2D1::Point2F(cx, cy)));
        rt->DrawBitmap(bitmap.Get(), D2D1::RectF(cx - dw / 2, cy - dh / 2, cx + dw / 2, cy + dh / 2), 1.0f,
                       zoom >= 4.0 ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->PopAxisAlignedClip();
    }
    bool loadingNow() const { return idx >= 0 && idx < (int)files.size() && (!cur || lowerW(cur->path) != lowerW(files[idx])); }

    void drawStatus() {
        RECT cl = client();
        int h = stH();
        if (!h) return;
        RECT sr = {0, cl.bottom - h, cl.right, cl.bottom};
        fillRect(sr, th.statusBg);
        std::string left;
        if (hotBtn >= 0 && hotBtn < (int)buttons.size()) left = hintFor(buttons[hotBtn].cmd);
        else if (loadingNow()) left = tr("status.loading", "...");
        std::vector<std::string> cells;
        std::vector<int> widths;
        if (cur && cur->ok && !loadingNow()) {
            cells.push_back(fmtArgs(tr("status.index", "%1/%2"), std::to_string(idx + 1), std::to_string(files.size())));
            widths.push_back(cfg.getInt("StatusBar/ViewerIndexWidth", 70));
            cells.push_back(sizeText(cur->fileSize));
            widths.push_back(cfg.getInt("StatusBar/ViewerSizeWidth", 80));
            cells.push_back(infoLine((int)cur->origW, (int)cur->origH, cur->bits, cur->format));
            widths.push_back(cfg.getInt("StatusBar/ViewerImageInfoWidth", 190));
            char zb[32];
            std::snprintf(zb, sizeof zb, "%.0f", zoom * 100.0);
            cells.push_back(fmtArgs(tr("status.zoom", "%1%"), zb));
            widths.push_back(cfg.getInt("StatusBar/ViewerZoomWidth", 60));
            char tb[32];
            std::snprintf(tb, sizeof tb, "%.2f", cur->secs);
            cells.push_back(fmtArgs(tr("status.loadTime", "%1"), tb));
            widths.push_back(cfg.getInt("StatusBar/ViewerLoadTimeWidth", 110));
        } else if (files.size() > 0 && idx >= 0) {
            cells.push_back(fmtArgs(tr("status.index", "%1/%2"), std::to_string(idx + 1), std::to_string(files.size())));
            widths.push_back(cfg.getInt("StatusBar/ViewerIndexWidth", 70));
        }
        int x = cl.right;
        for (int i = (int)cells.size() - 1; i >= 0; --i) {
            int w = S(widths[i]);
            RECT r = {x - w, sr.top, x, sr.bottom};
            drawText(cells[i], {r.left + S(6), r.top, r.right - S(2), r.bottom}, th.statusFg);
            x -= w;
        }
        drawText(left, {S(8), sr.top, std::max<LONG>(S(8), x - S(4)), sr.bottom}, th.statusFg);
    }
    void drawToolbar() {
        int h = tbH();
        if (!h) return;
        RECT cl = client();
        RECT tr_ = {0, 0, cl.right, h};
        fillRect(tr_, th.toolbarBg);
        int iconPx = S(std::max(12, std::min(64, cfg.getInt("Toolbar/IconSize", 16))));
        for (size_t i = 0; i < buttons.size(); ++i) {
            const Button& b = buttons[i];
            if (b.sep) {
                RECT r = {(b.r.left + b.r.right) / 2, b.r.top, (b.r.left + b.r.right) / 2 + 1, b.r.bottom};
                fillRect(r, th.border);
                continue;
            }
            bool hot = (int)i == hotBtn, down = (int)i == downBtn && hot;
            if (down) fillRect(b.r, th.accentSoft);
            else if (hot || checked(b.cmd)) fillRect(b.r, th.hover);
            Icon* ic = iconFor(b.cmd);
            if (ic) drawIcon(*ic, (float)(b.r.left + (b.r.right - b.r.left - iconPx) / 2), (float)(b.r.top + (b.r.bottom - b.r.top - iconPx) / 2), (float)iconPx);
        }
    }
    void paint() {
        if (!ensureTarget()) return;
        ensureBitmap();
        layoutToolbar();
        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        RECT cl = client();
        RECT cv = canvasRect();
        fillRect({0, 0, cl.right, cl.bottom}, th.canvas);
        if (hasImage()) {
            drawImage();
        } else {
            std::string msg;
            unsigned col = th.canvasErr;
            if (!runtimeErr.empty()) msg = runtimeErr;
            else if (cur && !cur->ok && !loadingNow()) msg = cur->err;
            else if (files.empty()) { msg = "按 Ctrl+O 打开图片,或把图片拖进窗口"; col = th.statusFg == th.canvas ? 0xFFFFFF : 0xB0B0B0; }
            if (!msg.empty()) drawText(msg, cv, col, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        drawToolbar();
        drawStatus();
        HRESULT hr = rt->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) dropTarget();
    }

    // ---------------- 视图:缩放 / 平移
    double computeFit() const {
        if (!cur || !cur->ok) return 1.0;
        RECT c = canvasRect();
        return fitZoom(effW(), effH(), (double)(c.right - c.left), (double)(c.bottom - c.top),
                       cfg.getBool("Viewer/ShrinkToWindow", true), cfg.getBool("Viewer/EnlargeToWindow", true));
    }
    void clampOffset() {
        if (!cur || !cur->ok) { offX = offY = 0; return; }
        RECT c = canvasRect();
        offX = clampPan(offX, effW() * zoom, (double)(c.right - c.left));
        offY = clampPan(offY, effH() * zoom, (double)(c.bottom - c.top));
    }
    bool canPan(bool horizontal) const {
        if (!cur || !cur->ok) return false;
        RECT c = canvasRect();
        return horizontal ? effW() * zoom > (c.right - c.left) + 0.5 : effH() * zoom > (c.bottom - c.top) + 0.5;
    }
    void refit() {
        if (!cur || !cur->ok) return;
        zoom = clampZoom(computeFit(), cfg.getDouble("Viewer/ZoomMin", 0.01), cfg.getDouble("Viewer/ZoomMax", 64.0));
        fitAuto = true;
        offX = offY = 0;
    }
    void setZoom(double z) {
        if (!cur || !cur->ok) return;
        z = clampZoom(z, cfg.getDouble("Viewer/ZoomMin", 0.01), cfg.getDouble("Viewer/ZoomMax", 64.0));
        if (std::fabs(z - zoom) < 1e-12) return;
        RECT c = canvasRect();
        double ax = 0, ay = 0;
        if (anchorValid) {
            ax = anchor.x - (c.left + (c.right - c.left) / 2.0);
            ay = anchor.y - (c.top + (c.bottom - c.top) / 2.0);
        }
        offX = zoomOffset(ax, offX, zoom, z);
        offY = zoomOffset(ay, offY, zoom, z);
        zoom = z;
        fitAuto = false;
        clampOffset();
        invalidate();
    }
    void panBy(double dx, double dy) {
        offX += dx;
        offY += dy;
        clampOffset();
        invalidate();
    }
    void arrow(int sx, int sy) {   // 方向键:放大后平移,否则翻页
        if (cfg.getBool("Viewer/ArrowPan", true) && hasImage()) {
            bool okx = sx != 0 && canPan(true), oky = sy != 0 && canPan(false);
            if (okx || oky) {
                double step = std::max(10, cfg.getInt("Viewer/PanStep", 120));
                panBy(okx ? -sx * step : 0, oky ? -sy * step : 0);
                return;
            }
        }
        go(idx + ((sx > 0 || sy > 0) ? 1 : -1));
    }
    void rotate(int dir) {
        if (!hasImage()) return;
        rot = (rot + (dir > 0 ? 1 : 3)) & 3;
        if (fitAuto) refit(); else clampOffset();
        invalidate();
    }

    // ---------------- 文件列表 / 翻页 / 载入
    bool isImageName(const std::wstring& n) const { return exts.count(extOf(n)) != 0; }

    void listDir(const std::wstring& dir) {
        files.clear();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (isImageName(fd.cFileName)) files.push_back(dir + L"\\" + fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(files.begin(), files.end(), [](const std::wstring& a, const std::wstring& b) {
            return StrCmpLogicalW(baseName(a).c_str(), baseName(b).c_str()) < 0;
        });
    }

    void openPath(std::wstring p) {
        p = absPath(p);
        std::wstring dir, target;
        if (dirExists(p)) { dir = p; }
        else if (fileExists(p)) { dir = parentDir(p); target = p; }
        else { return; }
        if (!cfg.getBool("Viewer/ViewAllInFolder", true) && !target.empty()) {
            files.assign(1, target);
        } else {
            listDir(dir);
            if (!target.empty()) {
                bool found = false;
                for (auto& f : files) if (lowerW(f) == lowerW(target)) { found = true; break; }
                if (!found) {   // 扩展名不在列表里的文件:加进去再排
                    files.push_back(target);
                    std::sort(files.begin(), files.end(), [](const std::wstring& a, const std::wstring& b) {
                        return StrCmpLogicalW(baseName(a).c_str(), baseName(b).c_str()) < 0;
                    });
                }
            }
        }
        if (files.empty()) { idx = -1; cur.reset(); bitmap.Reset(); updateTitle(); invalidate(); return; }
        idx = 0;
        if (!target.empty())
            for (size_t i = 0; i < files.size(); ++i) if (lowerW(files[i]) == lowerW(target)) { idx = (int)i; break; }
        showNow();
    }

    void go(int n) {
        int cnt = (int)files.size();
        if (cnt == 0) return;
        if (n < 0) { if (!cfg.getBool("Viewer/Wrap", true)) return; n = cnt - 1; }
        else if (n >= cnt) { if (!cfg.getBool("Viewer/Wrap", true)) return; n = 0; }
        if (n == idx) return;
        idx = n;
        updateTitle();
        invalidate();
        int delay = cfg.getInt("Viewer/FlipDecodeDelayMs", 60);
        DWORD now = GetTickCount();
        bool rapid = delay > 0 && (now - lastGo) < (DWORD)delay;
        lastGo = now;
        if (rapid) SetTimer(hwnd, TIMER_FLIP, (UINT)delay, nullptr);
        else { KillTimer(hwnd, TIMER_FLIP); showNow(); }
    }

    void request(const std::wstring& path, bool front) {
        if (cache.beginLoad(path)) pool.push(path, front);
    }

    void showNow() {
        if (idx < 0 || idx >= (int)files.size()) return;
        const std::wstring& p = files[idx];
        updateTitle();
        auto c = cache.get(p);
        if (c) present(c);
        else { request(p, true); invalidate(); }
    }

    void present(std::shared_ptr<Loaded> l) {
        bool hadImage = hasImage();
        firstShow = false;
        cur = l;
        bitmap.Reset();
        runtimeErr.clear();
        rot = 0;
        offX = offY = 0;
        if (l->ok && rt) ensureBitmap();
        if (l->ok && zoomLock && hadImage) { fitAuto = false; clampOffset(); }
        else refit();
        updateTitle();
        invalidate();
        int n = std::min(5, std::max(0, cfg.getInt("Viewer/PreloadCount", 1)));
        int cnt = (int)files.size();
        for (int k = 1; k <= n && cnt > 1; ++k) {
            request(files[(idx + k) % cnt], false);
            request(files[((idx - k) % cnt + cnt) % cnt], false);
        }
    }

    void onLoaded() {
        if (idx < 0 || idx >= (int)files.size()) return;
        if (cur && lowerW(cur->path) == lowerW(files[idx])) return;
        auto c = cache.get(files[idx]);
        if (!c) return;
        if (!c->ok && firstShow && tryPythonFor(files[idx])) return;
        firstShow = false;
        present(c);
    }
    bool firstShow = true;   // 第一张图读不了才交给 Python 版;之后翻页遇到坏图只显示错误

    bool tryPythonFor(const std::wstring& path) {   // 第一张就读不了:交给 Python 版(它支持 RAW/PSD/SVG/HDR…)
        if (nat.get("Native/PythonFallback", "1") == "0") return false;
        if (!pythonAvailable(root)) return false;
        if (!launchPython(root, {path})) return false;
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return true;
    }

    void updateTitle() {
        std::string name = "Xview";
        if (idx >= 0 && idx < (int)files.size()) {
            std::wstring p = cfg.getBool("Viewer/ShowFullPath", true) ? files[idx] : baseName(files[idx]);
            name = fmtArgs(tr("app.titleFormat", "%1 - Xview"), U(p));
        }
        SetWindowTextW(hwnd, W(name).c_str());
    }

    // ---------------- 命令
    void closeWindow() { PostMessageW(hwnd, WM_CLOSE, 0, 0); }

    void setTopmost(bool on) {
        topmost = on;
        SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void showCursor(bool show) {
        if (show && cursorHidden) { ShowCursor(TRUE); cursorHidden = false; }
        else if (!show && !cursorHidden) { ShowCursor(FALSE); cursorHidden = true; }
    }

    void setFullscreen(bool on) {
        if (on == fullscreen) return;
        if (on) {
            prevStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
            prevPlace.length = sizeof prevPlace;
            GetWindowPlacement(hwnd, &prevPlace);
            MONITORINFO mi;
            mi.cbSize = sizeof mi;
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            SetWindowLongPtrW(hwnd, GWL_STYLE, prevStyle & ~(LONG_PTR)WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            fullscreen = true;
            SetTimer(hwnd, TIMER_CURSOR, 400, nullptr);
        } else {
            fullscreen = false;
            KillTimer(hwnd, TIMER_CURSOR);
            showCursor(true);
            SetWindowLongPtrW(hwnd, GWL_STYLE, prevStyle);
            SetWindowPlacement(hwnd, &prevPlace);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            if (topmost) setTopmost(true);
        }
        if (fitAuto) refit(); else clampOffset();
        invalidate();
    }

    void openDialog() {
        wchar_t buf[MAX_PATH * 4] = L"";
        std::wstring filter = L"图片";
        filter.push_back(L'\0');
        std::wstring pats;
        for (auto& e : exts) { if (!pats.empty()) pats += L";"; pats += L"*." + e; }
        filter += pats;
        filter.push_back(L'\0');
        filter += L"所有文件";
        filter.push_back(L'\0');
        filter += L"*.*";
        filter.push_back(L'\0');
        OPENFILENAMEW ofn;
        ZeroMemory(&ofn, sizeof ofn);
        ofn.lStructSize = sizeof ofn;
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = filter.c_str();
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH * 4;
        std::wstring initDir = (idx >= 0 && idx < (int)files.size()) ? parentDir(files[idx]) : L"";
        ofn.lpstrInitialDir = initDir.empty() ? nullptr : initDir.c_str();
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_HIDEREADONLY;
        if (GetOpenFileNameW(&ofn)) openPath(buf);
    }

    void setClipboardText(const std::wstring& s) {
        if (!OpenClipboard(hwnd)) return;
        EmptyClipboard();
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
        if (g) {
            memcpy(GlobalLock(g), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(g);
            if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
        }
        CloseClipboard();
    }

    void copyImage() {
        if (!cur || !cur->ok) return;
        UINT w = cur->texW, h = cur->texH;
        UINT stride = w * 4;
        size_t total = (size_t)stride * h;
        std::vector<BYTE> px(total);
        WICRect rc = {0, 0, (INT)w, (INT)h};
        if (FAILED(cur->bmp->CopyPixels(&rc, stride, (UINT)total, px.data()))) return;
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + total);
        if (!g) return;
        BYTE* m = (BYTE*)GlobalLock(g);
        BITMAPINFOHEADER bi;
        ZeroMemory(&bi, sizeof bi);
        bi.biSize = sizeof bi;
        bi.biWidth = (LONG)w;
        bi.biHeight = (LONG)h;   // 正数 = 自下而上
        bi.biPlanes = 1;
        bi.biBitCount = 32;
        bi.biCompression = BI_RGB;
        memcpy(m, &bi, sizeof bi);
        BYTE* dst = m + sizeof bi;
        for (UINT y = 0; y < h; ++y) {
            BYTE* d = dst + (size_t)(h - 1 - y) * stride;
            memcpy(d, px.data() + (size_t)y * stride, stride);
            for (UINT x = 0; x < w; ++x) d[x * 4 + 3] = 0xFF;
        }
        GlobalUnlock(g);
        if (OpenClipboard(hwnd)) {
            EmptyClipboard();
            if (!SetClipboardData(CF_DIB, g)) GlobalFree(g);
            CloseClipboard();
        } else {
            GlobalFree(g);
        }
    }

    void deleteCurrent() {
        if (idx < 0 || idx >= (int)files.size()) return;
        std::wstring p = files[idx];
        if (cfg.get("Archive/DeleteMode", "Confirm") == "Confirm") {
            std::wstring msg = L"把这个文件移到回收站?\n\n" + p;
            if (MessageBoxW(hwnd, msg.c_str(), L"Xview", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        }
        std::wstring from = p;
        from.push_back(L'\0');
        SHFILEOPSTRUCTW op;
        ZeroMemory(&op, sizeof op);
        op.hwnd = hwnd;
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
        if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted) return;
        cache.erase(p);
        files.erase(files.begin() + idx);
        cur.reset();
        bitmap.Reset();
        if (files.empty()) { idx = -1; updateTitle(); invalidate(); return; }
        if (idx >= (int)files.size()) idx = (int)files.size() - 1;
        showNow();
    }

    void showProperties() {
        if (idx < 0 || idx >= (int)files.size()) return;
        SHELLEXECUTEINFOW sei;
        ZeroMemory(&sei, sizeof sei);
        sei.cbSize = sizeof sei;
        sei.fMask = SEE_MASK_INVOKEIDLIST;
        sei.hwnd = hwnd;
        sei.lpVerb = L"properties";
        sei.lpFile = files[idx].c_str();
        sei.nShow = SW_SHOW;
        ShellExecuteExW(&sei);
    }

    void run(const std::string& c) {
        if (c.empty() || c == "None") return;
        if (c == "BackToBrowser" || c == "CloseImage" || c == "Exit" || c == "QuitBrowser") closeWindow();
        else if (c == "NextImage") go(idx + 1);
        else if (c == "PrevImage") go(idx - 1);
        else if (c == "FirstImage") go(0);
        else if (c == "LastImage") go((int)files.size() - 1);
        else if (c == "ZoomIn") setZoom(zoom * std::max(1.01, cfg.getDouble("Viewer/ZoomStep", 1.25)));
        else if (c == "ZoomOut") setZoom(zoom / std::max(1.01, cfg.getDouble("Viewer/ZoomStep", 1.25)));
        else if (c == "FitWindow") { refit(); invalidate(); }
        else if (c == "ActualSize") setZoom(1.0);
        else if (c == "ZoomLock") { zoomLock = !zoomLock; invalidate(); }
        else if (c == "RotateLeft") rotate(-1);
        else if (c == "RotateRight") rotate(1);
        else if (c == "Fullscreen") setFullscreen(!fullscreen);
        else if (c == "AlwaysOnTop") setTopmost(!topmost);
        else if (c == "ReloadImage") {
            if (idx >= 0 && idx < (int)files.size()) { cache.erase(files[idx]); cur.reset(); bitmap.Reset(); showNow(); }
        }
        else if (c == "OpenFile") openDialog();
        else if (c == "CopyImage") copyImage();
        else if (c == "CopyPath") { if (idx >= 0 && idx < (int)files.size()) setClipboardText(files[idx]); }
        else if (c == "ShowInExplorer") {
            if (idx >= 0 && idx < (int)files.size()) {
                std::wstring arg = L"/select," + quoteArg(files[idx]);
                ShellExecuteW(hwnd, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
            }
        }
        else if (c == "Properties") showProperties();
        else if (c == "Delete") deleteCurrent();
        else if (c == "ToggleToolbar") { showToolbar = !showToolbar; if (fitAuto) refit(); else clampOffset(); invalidate(); }
        else if (c == "ToggleStatusBar") { showStatus = !showStatus; if (fitAuto) refit(); else clampOffset(); invalidate(); }
        else if (c == "ArrowLeft") arrow(-1, 0);
        else if (c == "ArrowRight") arrow(1, 0);
        else if (c == "ArrowUp") arrow(0, -1);
        else if (c == "ArrowDown") arrow(0, 1);
    }

    void contextMenu(POINT screenPt) {
        HMENU m = CreatePopupMenu();
        std::vector<std::string> ids;
        bool lastSep = true;
        for (auto& tok : cfg.getList("ContextMenu/Viewer")) {
            if (tok == "|") {
                if (!lastSep) { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); lastSep = true; }
                continue;
            }
            if (!implemented(tok) || tok.compare(0, 5, "Arrow") == 0) continue;
            std::wstring label = W(cmdLabel(tok));
            auto k = cfg.getList("Keys/" + tok);
            if (!k.empty()) label += L"\t" + W(k[0]);
            AppendMenuW(m, MF_STRING | (checked(tok) ? MF_CHECKED : 0), (UINT_PTR)(1000 + ids.size()), label.c_str());
            ids.push_back(tok);
            lastSep = false;
        }
        if (lastSep && GetMenuItemCount(m) > 0) RemoveMenu(m, (UINT)GetMenuItemCount(m) - 1, MF_BYPOSITION);
        SetForegroundWindow(hwnd);
        int sel = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, 0, hwnd, nullptr);
        DestroyMenu(m);
        if (sel >= 1000 && sel - 1000 < (int)ids.size()) run(ids[(size_t)(sel - 1000)]);
    }

    // ---------------- 窗口位置记忆
    bool parseRect4(const std::string& v, int* o, int n) {
        auto parts = splitList(v);
        if ((int)parts.size() < n) return false;
        for (int i = 0; i < n; ++i) o[i] = std::atoi(parts[(size_t)i].c_str());
        return true;
    }
    void initialRect(int& x, int& y, int& w, int& h, bool& maximized) {
        int v[5] = {0, 0, 0, 0, 0};
        maximized = false;
        bool have = false;
        if (cfg.getBool("Viewer/SaveWindowPos", true)) {
            if (parseRect4(nat.get("Native/Window"), v, 4)) { have = true; parseRect4(nat.get("Native/Window"), v, 5); maximized = v[4] != 0; }
            else {
                Ini st;
                st.loadText(readFileText(root + L"\\data\\config\\state.conf"));
                if (parseRect4(st.get("/win.viewer"), v, 4)) have = true;
            }
        }
        if (have && v[2] > 100 && v[3] > 100) {
            RECT r = {v[0], v[1], v[0] + v[2], v[1] + v[3]};
            if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) { x = v[0]; y = v[1]; w = v[2]; h = v[3]; return; }
        }
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        w = std::min<int>(S(1100), (wa.right - wa.left) * 8 / 10);
        h = std::min<int>(S(800), (wa.bottom - wa.top) * 8 / 10);
        x = wa.left + ((wa.right - wa.left) - w) / 2;
        y = wa.top + ((wa.bottom - wa.top) - h) / 2;
    }
    void saveState() {
        if (!cfg.getBool("Viewer/SaveWindowPos", true)) return;
        if (fullscreen) setFullscreen(false);
        WINDOWPLACEMENT wp;
        wp.length = sizeof wp;
        if (!GetWindowPlacement(hwnd, &wp)) return;
        RECT r = wp.rcNormalPosition;
        std::string v = std::to_string(r.left) + "," + std::to_string(r.top) + "," + std::to_string(r.right - r.left) + "," + std::to_string(r.bottom - r.top) + "," + (wp.showCmd == SW_SHOWMAXIMIZED ? "1" : "0");
        nat.kv["Native/Window"] = v;
        std::string out = "; Xview 原生版自己的记忆(窗口位置)。可以删除,不影响其他设置。\n; PythonFallback=0 可关闭\"读不了的格式交给 Python 版\"\n[Native]\n";
        for (auto& p : nat.kv)
            if (p.first.compare(0, 7, "Native/") == 0) out += p.first.substr(7) + "=" + p.second + "\n";
        CreateDirectoryW((root + L"\\data").c_str(), nullptr);
        CreateDirectoryW((root + L"\\data\\config").c_str(), nullptr);
        writeFileText(root + L"\\data\\config\\native.ini", out);
    }
};

App* g = nullptr;

void poolWork(const std::wstring& path) {
    auto r = decodeFile(path);
    g->cache.put(path, r);
    g->cache.endLoad(path);
    if (g->hwnd) PostMessageW(g->hwnd, WM_APP_LOADED, 0, 0);
}

int modsNow() {
    return ((GetKeyState(VK_CONTROL) & 0x8000) ? KM_CTRL : 0) | ((GetKeyState(VK_MENU) & 0x8000) ? KM_ALT : 0) | ((GetKeyState(VK_SHIFT) & 0x8000) ? KM_SHIFT : 0);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* a = g;
    if (!a) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            a->paint();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SIZE: {
            if (a->rt && wp != SIZE_MINIMIZED) a->rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            if (wp != SIZE_MINIMIZED) {
                if (a->fitAuto) a->refit(); else a->clampOffset();
                a->invalidate();
            }
            return 0;
        }
        case WM_DPICHANGED: {
            a->dpi = HIWORD(wp);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            a->makeTextFormat();
            a->invalidate();
            return 0;
        }
        case WM_APP_LOADED:
            a->onLoaded();
            return 0;
        case WM_TIMER:
            if (wp == TIMER_FLIP) { KillTimer(hwnd, TIMER_FLIP); a->showNow(); }
            else if (wp == TIMER_CURSOR) {
                if (a->fullscreen && a->cfg.getBool("Viewer/HideMouseInFullscreen", true) && GetTickCount() - a->lastMouse > 1500) a->showCursor(false);
            }
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const std::string* c = a->keys.byVk((int)wp, modsNow());
            if (c) { a->handledKey = true; std::string cmd = *c; a->run(cmd); return 0; }
            a->handledKey = false;
            break;
        }
        case WM_CHAR: {
            if (a->handledKey) { a->handledKey = false; return 0; }
            if (wp > 32 && wp < 127 && !(modsNow() & (KM_CTRL | KM_ALT))) {
                const std::string* c = a->keys.byChar((char)wp);
                if (c) { std::string cmd = *c; a->run(cmd); return 0; }
            }
            break;
        }
        case WM_MOUSEWHEEL: {
            int d = GET_WHEEL_DELTA_WPARAM(wp);
            if (d == 0) return 0;
            std::string key = std::string((modsNow() & KM_CTRL) ? "CtrlWheel." : "Wheel.") + (d > 0 ? "Up" : "Down");
            POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &p);
            a->anchor = p;
            a->anchorValid = true;
            std::string cmd = a->cfg.get("Mouse/" + key);
            a->run(cmd);
            a->anchorValid = false;
            return 0;
        }
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            int b = a->buttonAt(x, y);
            if (b >= 0) { a->downBtn = b; SetCapture(hwnd); a->invalidate(); return 0; }
            if (y >= a->tbH() && a->cfg.get("Mouse/LeftDrag", "Pan") == "Pan" && a->hasImage() && (a->canPan(true) || a->canPan(false))) {
                a->dragging = true;
                a->dragLast = {x, y};
                SetCapture(hwnd);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            a->lastMouse = GetTickCount();
            a->showCursor(true);
            if (!a->trackingLeave) {
                TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&t);
                a->trackingLeave = true;
            }
            if (a->dragging) {
                a->panBy(x - a->dragLast.x, y - a->dragLast.y);
                a->dragLast = {x, y};
            } else {
                int b = a->buttonAt(x, y);
                if (b != a->hotBtn) { a->hotBtn = b; a->invalidate(); }
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            a->trackingLeave = false;
            if (a->hotBtn != -1) { a->hotBtn = -1; a->invalidate(); }
            return 0;
        case WM_LBUTTONUP: {
            if (a->dragging) { a->dragging = false; ReleaseCapture(); return 0; }
            if (a->downBtn >= 0) {
                int b = a->buttonAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                int down = a->downBtn;
                a->downBtn = -1;
                ReleaseCapture();
                a->invalidate();
                if (b == down && b < (int)a->buttons.size()) { std::string cmd = a->buttons[(size_t)b].cmd; a->run(cmd); }
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            if (GET_Y_LPARAM(lp) >= a->tbH()) a->run(a->cfg.get("Mouse/DoubleClick"));
            return 0;
        }
        case WM_RBUTTONUP: {
            std::string c = a->cfg.get("Mouse/RightClick", "ContextMenu");
            if (c == "ContextMenu") { POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; ClientToScreen(hwnd, &p); a->contextMenu(p); }
            else a->run(c);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(hwnd, &p);
                bool onCanvas = p.y >= a->tbH();
                if (a->cursorHidden) { SetCursor(nullptr); return TRUE; }
                if (onCanvas && a->hasImage() && (a->dragging || a->canPan(true) || a->canPan(false))) { SetCursor(LoadCursorW(nullptr, IDC_SIZEALL)); return TRUE; }
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_DROPFILES: {
            wchar_t buf[MAX_PATH * 4];
            if (DragQueryFileW((HDROP)wp, 0, buf, MAX_PATH * 4)) a->openPath(buf);
            DragFinish((HDROP)wp);
            return 0;
        }
        case WM_COPYDATA: {
            auto* cd = reinterpret_cast<COPYDATASTRUCT*>(lp);
            if (cd && cd->dwData == kCopyDataPath && cd->lpData && cd->cbData >= sizeof(wchar_t)) {
                std::wstring p((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t));
                while (!p.empty() && p.back() == L'\0') p.pop_back();
                if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
                SetForegroundWindow(hwnd);
                if (!p.empty()) a->openPath(p);
                return TRUE;
            }
            return FALSE;
        }
        case WM_CLOSE:
            a->saveState();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> paths, flags;
    bool newWindow = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring s = argv[i];
        if (s == L"--new-window") newWindow = true;
        else if (s.rfind(L"--", 0) == 0) flags.push_back(s);
        else paths.push_back(absPath(s));
    }
    LocalFree(argv);
    std::wstring root = exeDir();

    // "--warm" = 登录预热:原生版本来就很快,什么也不用做
    if (std::find(flags.begin(), flags.end(), L"--warm") != flags.end() && flags.size() == 1) return 0;
    // 其他 "--xxx"(选项对话框用的 --list-formats / --assoc-machine 等):交给 Python 版
    if (!flags.empty()) {
        std::vector<std::wstring> all = flags;
        for (auto& p : paths) all.push_back(p);
        if (launchPython(root, all)) return 0;
        MessageBoxW(nullptr, L"这个命令行选项需要 Python 版(找不到 pythonw.exe 或 app\\src\\main.pyw)。", L"Xview", MB_ICONERROR);
        return 2;
    }
    // 已有窗口:把路径交给它
    if (!newWindow) {
        HWND other = FindWindowW(kClass, nullptr);
        if (other) {
            if (!paths.empty()) {
                COPYDATASTRUCT cd;
                cd.dwData = kCopyDataPath;
                cd.cbData = (DWORD)((paths[0].size() + 1) * sizeof(wchar_t));
                cd.lpData = (PVOID)paths[0].c_str();
                SendMessageW(other, WM_COPYDATA, 0, (LPARAM)&cd);
            } else {
                if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
                SetForegroundWindow(other);
            }
            return 0;
        }
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic)))) {
        MessageBoxW(nullptr, L"初始化图像组件(WIC)失败。", L"Xview", MB_ICONERROR);
        return 1;
    }

    g = new App();   // 故意不释放:退出时直接 ExitProcess,解码线程不会碰到已销毁的对象
    App& a = *g;
    a.root = root;
    a.loadConfig();
    if (!a.initGraphics()) {
        MessageBoxW(nullptr, L"初始化 Direct2D 失败。", L"Xview", MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    a.dpi = 96;
    HDC sdc = GetDC(nullptr);
    if (sdc) { a.dpi = GetDeviceCaps(sdc, LOGPIXELSX); ReleaseDC(nullptr, sdc); }
    int x, y, w, h;
    bool maxed;
    a.initialRect(x, y, w, h, maxed);
    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, kClass, L"Xview", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    a.hwnd = hwnd;
    a.dpi = (int)GetDpiForWindow(hwnd);
    a.makeTextFormat();
    DragAcceptFiles(hwnd, TRUE);
    a.pool.start(2, poolWork);
    if (a.topmost) a.setTopmost(true);
    ShowWindow(hwnd, maxed ? SW_SHOWMAXIMIZED : SW_SHOW);
    UpdateWindow(hwnd);
    if (a.cfg.getBool("Viewer/Fullscreen", false)) a.setFullscreen(true);

    if (!paths.empty()) a.openPath(paths[0]);
    else a.updateTitle();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    ExitProcess((UINT)m.wParam);
    return 0;
}
