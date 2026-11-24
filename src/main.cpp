// Xview 原生版:纯 C++ / Win32 / Direct2D / WIC 看图程序。
//  - 解码:WIC(系统自带,JPG PNG BMP GIF TIFF ICO,以及系统装了编码器的 WebP HEIF AVIF 等)
//    自己解码(codecs.h,不依赖任何别的程序):PSD/PSB、HDR、EXR、相机 RAW 的内嵌预览;
//    SVG/SVGZ 用 Direct2D;AI/EPS/CDR 向 Windows 要预览图
//  - 显示:Direct2D(GPU 贴图,缩放平移不重新解码)
//  - 配置:全部读外置文件(app\config\*、data\config\*),本程序只写 data\config\native.ini
//  - 没有任何 Python 回退:读不了的图只显示错误提示。
// 目录布局:Xview.exe 放在项目根,旁边是 app\ 和 data\。
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <d2d1_3.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <sddl.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <winspool.h>
#include <oleauto.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>
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

#include "codecs.h"
#include "ini.h"
#include "json.h"
#include "keymap.h"
#include "layout.h"
#include "svgpath.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "advapi32.lib")

using Microsoft::WRL::ComPtr;
using namespace xv;

namespace {

constexpr wchar_t kClass[] = L"XviewNativeWnd";
constexpr UINT WM_APP_LOADED = WM_APP + 1;
constexpr UINT_PTR TIMER_FLIP = 1;
constexpr UINT_PTR TIMER_CURSOR = 2;
constexpr UINT_PTR TIMER_SHOW = 3;    // 幻灯片放映
constexpr UINT_PTR TIMER_ANIM = 4;    // GIF 动画
constexpr UINT WM_APP_THUMB = WM_APP + 2;   // 胶片条缩略图生成好了
constexpr UINT ID_MENU_BASE = 2000;   // 菜单栏 / 弹出菜单的命令编号起点
constexpr ULONG_PTR kCopyDataPathAdd = 0x58564E32;   // 'XVN2':和 XVN1 一样交一个路径,但明确要求"添加到列表"
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
    std::vector<ComPtr<IWICBitmap>> frames;   // GIF 动画:每帧都是合成好的整幅画面(预乘 BGRA);只有 1 帧或不是 GIF 时为空
    std::vector<int> delays;                  // 每帧停留的毫秒数
};

ComPtr<IWICImagingFactory> g_wic;

// 解码线程要读的几个选项(主线程载入设置时写)
std::atomic<int> g_rawMode{0};        // Viewer/RawMode:0=Auto 1=Preview 2=Develop
std::atomic<double> g_gamma{0.0};     // Display/Gamma(0 = 不校正)
std::atomic<bool> g_gifOff{false};    // Display/DisableGifAnimation

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

bool isRawExt(const std::wstring& e) {
    static const wchar_t* k[] = {L"cr2", L"cr3", L"crw", L"nef", L"nrw", L"arw", L"srf", L"sr2", L"dng", L"orf", L"rw2", L"raf",
                                 L"pef", L"srw", L"x3f", L"3fr", L"erf", L"kdc", L"mrw", L"raw"};
    for (auto x : k) if (e == x) return true;
    return false;
}

bool readAllBytes(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 0x7FFFFFFFLL * 2) { CloseHandle(h); return false; }
    try { out.resize((size_t)sz.QuadPart); } catch (...) { CloseHandle(h); return false; }
    size_t done = 0;
    bool ok = true;
    while (done < out.size()) {
        DWORD got = 0, want = (DWORD)std::min<size_t>(out.size() - done, 64u << 20);
        if (!ReadFile(h, out.data() + done, want, &got, nullptr) || got == 0) { ok = false; break; }
        done += got;
    }
    CloseHandle(h);
    return ok;
}

// ---- SVG:用 Direct2D 自带的 SVG 渲染(Windows 10 1703 以上)。D2D 设备上下文不能多线程同时用,所以整个渲染过程加锁。
struct SvgCtx {
    std::mutex m;
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID2D1Factory1> fac;
    ComPtr<ID2D1Device> dev;
    ComPtr<ID2D1DeviceContext5> dc;
    bool tried = false, ok = false;
};
SvgCtx g_svg;

bool svgInit() {
    if (g_svg.tried) return g_svg.ok;
    g_svg.tried = true;
    UINT fl = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, fl, nullptr, 0, D3D11_SDK_VERSION, &g_svg.d3d, nullptr, nullptr);
    if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, fl, nullptr, 0, D3D11_SDK_VERSION, &g_svg.d3d, nullptr, nullptr);
    if (FAILED(hr)) return false;
    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(g_svg.d3d.As(&dxgi))) return false;
    D2D1_FACTORY_OPTIONS opt;
    ZeroMemory(&opt, sizeof opt);
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &opt, reinterpret_cast<void**>(g_svg.fac.GetAddressOf())))) return false;
    if (FAILED(g_svg.fac->CreateDevice(dxgi.Get(), &g_svg.dev))) return false;
    ComPtr<ID2D1DeviceContext> dc1;
    if (FAILED(g_svg.dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc1))) return false;
    if (FAILED(dc1.As(&g_svg.dc))) return false;   // 没有 ID2D1DeviceContext5 = 系统太旧,不支持 SVG
    g_svg.ok = true;
    return true;
}

bool renderSvg(const std::vector<uint8_t>& data, ComPtr<IWICBitmap>& out, UINT& outW, UINT& outH) {
    std::lock_guard<std::mutex> lk(g_svg.m);
    if (!svgInit()) return false;
    ComPtr<IStream> st;
    st.Attach(SHCreateMemStream(data.data(), (UINT)data.size()));
    if (!st) return false;
    ComPtr<ID2D1SvgDocument> doc;
    if (FAILED(g_svg.dc->CreateSvgDocument(st.Get(), D2D1::SizeF(1024.0f, 1024.0f), &doc)) || !doc) return false;
    ComPtr<ID2D1SvgElement> root;
    doc->GetRoot(&root);
    if (!root) return false;
    D2D1_SVG_VIEWBOX vb;
    ZeroMemory(&vb, sizeof vb);
    bool hasVb = root->IsAttributeSpecified(L"viewBox", nullptr) &&
                 SUCCEEDED(root->GetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &vb, sizeof vb)) && vb.width > 0 && vb.height > 0;
    if (!hasVb) {   // 没有 viewBox:用 width/height(纯数字)当画布,都没有就按 1024 见方
        float vw = 1024.0f, vh = 1024.0f;
        D2D1_SVG_LENGTH lw, lh;
        ZeroMemory(&lw, sizeof lw);
        ZeroMemory(&lh, sizeof lh);
        if (root->IsAttributeSpecified(L"width", nullptr) && SUCCEEDED(root->GetAttributeValue(L"width", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &lw, sizeof lw)) &&
            lw.units == D2D1_SVG_LENGTH_UNITS_NUMBER && lw.value > 0) vw = lw.value;
        if (root->IsAttributeSpecified(L"height", nullptr) && SUCCEEDED(root->GetAttributeValue(L"height", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &lh, sizeof lh)) &&
            lh.units == D2D1_SVG_LENGTH_UNITS_NUMBER && lh.value > 0) vh = lh.value;
        vb.x = 0; vb.y = 0; vb.width = vw; vb.height = vh;
        root->SetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &vb, sizeof vb);
    }
    const double kSide = 2048.0;   // 长边渲染到 2048 像素,放大看也清楚
    UINT W, H;
    if (vb.width >= vb.height) { W = (UINT)kSide; H = (UINT)std::max(1.0, std::floor(kSide * vb.height / vb.width + 0.5)); }
    else { H = (UINT)kSide; W = (UINT)std::max(1.0, std::floor(kSide * vb.width / vb.height + 0.5)); }
    D2D1_SVG_LENGTH nl;
    nl.value = (float)W; nl.units = D2D1_SVG_LENGTH_UNITS_NUMBER;
    root->SetAttributeValue(L"width", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &nl, sizeof nl);
    nl.value = (float)H;
    root->SetAttributeValue(L"height", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &nl, sizeof nl);
    doc->SetViewportSize(D2D1::SizeF((float)W, (float)H));

    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap1> tgt;
    if (FAILED(g_svg.dc->CreateBitmap(D2D1::SizeU(W, H), nullptr, 0, bp, &tgt))) return false;
    g_svg.dc->SetTarget(tgt.Get());
    g_svg.dc->BeginDraw();
    g_svg.dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    g_svg.dc->DrawSvgDocument(doc.Get());
    HRESULT hr = g_svg.dc->EndDraw();
    g_svg.dc->SetTarget(nullptr);
    if (FAILED(hr)) return false;
    D2D1_BITMAP_PROPERTIES1 sp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap1> stg;
    if (FAILED(g_svg.dc->CreateBitmap(D2D1::SizeU(W, H), nullptr, 0, sp, &stg))) return false;
    if (FAILED(stg->CopyFromBitmap(nullptr, tgt.Get(), nullptr))) return false;
    D2D1_MAPPED_RECT mr;
    if (FAILED(stg->Map(D2D1_MAP_OPTIONS_READ, &mr))) return false;
    std::vector<uint8_t> px((size_t)W * H * 4);
    for (UINT y = 0; y < H; ++y) std::memcpy(&px[(size_t)y * W * 4], mr.bits + (size_t)y * mr.pitch, (size_t)W * 4);
    stg->Unmap();
    if (FAILED(g_wic->CreateBitmapFromMemory(W, H, GUID_WICPixelFormat32bppPBGRA, W * 4, (UINT)px.size(), px.data(), &out))) return false;
    outW = W;
    outH = H;
    return true;
}

// ---- 向 Windows 要预览图:AI / EPS / CDR 这类自己不画的格式(装了 Illustrator / CorelDRAW 之后系统才认得)
bool shellThumb(const std::wstring& path, ComPtr<IWICBitmap>& out, UINT& outW, UINT& outH) {
    ComPtr<IShellItemImageFactory> f;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f))) || !f) return false;
    HBITMAP hb = nullptr;
    SIZE sz;
    sz.cx = 2048;
    sz.cy = 2048;
    if (FAILED(f->GetImage(sz, SIIGBF_BIGGERSIZEOK | SIIGBF_THUMBNAILONLY, &hb)) || !hb) return false;
    HRESULT hr = g_wic->CreateBitmapFromHBITMAP(hb, nullptr, WICBitmapUseAlpha, &out);
    DeleteObject(hb);
    if (FAILED(hr) || !out) return false;
    out->GetSize(&outW, &outH);
    return outW > 0 && outH > 0;
}

// γ 校正:输出 = 255 * (输入/255)^(1/γ)(γ>1 变亮)。返回 nullptr 表示不需要校正(或失败,调用者保留原图)
ComPtr<IWICBitmap> applyGamma(IWICBitmap* in, double gamma) {
    if (!in || gamma <= 0.0 || std::fabs(gamma - 1.0) < 1e-6) return nullptr;
    UINT w = 0, h = 0;
    in->GetSize(&w, &h);
    if (!w || !h) return nullptr;
    const UINT stride = w * 4;
    std::vector<BYTE> px((size_t)stride * h);
    WICRect rc = {0, 0, (INT)w, (INT)h};
    if (FAILED(in->CopyPixels(&rc, stride, (UINT)px.size(), px.data()))) return nullptr;
    BYTE lut[256];
    for (int i = 0; i < 256; ++i) lut[i] = (BYTE)std::min(255L, std::lround(255.0 * std::pow(i / 255.0, 1.0 / gamma)));
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        BYTE a = px[i + 3];
        if (a == 255) { px[i] = lut[px[i]]; px[i + 1] = lut[px[i + 1]]; px[i + 2] = lut[px[i + 2]]; }
        else if (a > 0) {   // 预乘的像素:先还原、校正、再乘回去
            for (int c = 0; c < 3; ++c) {
                unsigned v = std::min(255u, (unsigned)px[i + c] * 255u / a);
                px[i + c] = (BYTE)((unsigned)lut[v] * a / 255u);
            }
        }
    }
    ComPtr<IWICBitmap> out;
    if (FAILED(g_wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppPBGRA, stride, (UINT)px.size(), px.data(), &out))) return nullptr;
    return out;
}

UINT metaUInt(IWICMetadataQueryReader* q, const wchar_t* name, UINT def) {
    if (!q) return def;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    UINT v = def;
    if (SUCCEEDED(q->GetMetadataByName(name, &pv))) {
        if (pv.vt == VT_UI2) v = pv.uiVal;
        else if (pv.vt == VT_UI1) v = pv.bVal;
        else if (pv.vt == VT_UI4) v = pv.ulVal;
    }
    PropVariantClear(&pv);
    return v;
}

// GIF 动画:把每一帧按"处置方式"合成到整幅画布上。帧数 <2、太大、读不出来都返回 false(调用者按普通静态图处理)
bool decodeGifFrames(IWICBitmapDecoder* dec, Loaded& r, UINT& cw, UINT& ch) {
    UINT fc = 0;
    if (FAILED(dec->GetFrameCount(&fc)) || fc < 2) return false;
    ComPtr<IWICMetadataQueryReader> dq;
    dec->GetMetadataQueryReader(&dq);
    cw = metaUInt(dq.Get(), L"/logscrdesc/Width", 0);
    ch = metaUInt(dq.Get(), L"/logscrdesc/Height", 0);
    if (!cw || !ch) {
        ComPtr<IWICBitmapFrameDecode> f0;
        if (FAILED(dec->GetFrame(0, &f0)) || FAILED(f0->GetSize(&cw, &ch))) return false;
    }
    if (!cw || !ch || cw > 16384 || ch > 16384) return false;
    const size_t frameBytes = (size_t)cw * ch * 4;
    if (frameBytes * fc > ((size_t)384 << 20)) return false;   // 帧太多太大:只显示第一帧,免得吃光内存
    std::vector<BYTE> canvas(frameBytes, 0), saved;
    std::vector<ComPtr<IWICBitmap>> frames;
    std::vector<int> delays;
    for (UINT i = 0; i < fc; ++i) {
        ComPtr<IWICBitmapFrameDecode> f;
        if (FAILED(dec->GetFrame(i, &f))) return false;
        ComPtr<IWICMetadataQueryReader> mq;
        f->GetMetadataQueryReader(&mq);
        UINT left = metaUInt(mq.Get(), L"/imgdesc/Left", 0), top = metaUInt(mq.Get(), L"/imgdesc/Top", 0);
        UINT delay = metaUInt(mq.Get(), L"/grctlext/Delay", 10), disp = metaUInt(mq.Get(), L"/grctlext/Disposal", 0);
        UINT fw = 0, fh = 0;
        if (FAILED(f->GetSize(&fw, &fh)) || !fw || !fh) return false;
        ComPtr<IWICFormatConverter> cv;
        if (FAILED(g_wic->CreateFormatConverter(&cv))) return false;
        if (FAILED(cv->Initialize(f.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut))) return false;
        std::vector<BYTE> fp((size_t)fw * fh * 4);
        WICRect rc = {0, 0, (INT)fw, (INT)fh};
        if (FAILED(cv->CopyPixels(&rc, fw * 4, (UINT)fp.size(), fp.data()))) return false;
        if (disp == 3) saved = canvas;
        for (UINT y = 0; y < fh && top + y < ch; ++y) {
            for (UINT x = 0; x < fw && left + x < cw; ++x) {
                const BYTE* sp = &fp[((size_t)y * fw + x) * 4];
                BYTE* dp = &canvas[((size_t)(top + y) * cw + (left + x)) * 4];
                BYTE sa = sp[3];
                if (sa == 255) std::memcpy(dp, sp, 4);
                else if (sa) { unsigned inv = 255u - sa; for (int c = 0; c < 4; ++c) dp[c] = (BYTE)std::min(255u, sp[c] + (dp[c] * inv + 127u) / 255u); }
            }
        }
        ComPtr<IWICBitmap> snap;
        if (FAILED(g_wic->CreateBitmapFromMemory(cw, ch, GUID_WICPixelFormat32bppPBGRA, cw * 4, (UINT)frameBytes, canvas.data(), &snap))) return false;
        frames.push_back(snap);
        delays.push_back(delay <= 1 ? 100 : (int)delay * 10);   // 浏览器的惯例:10 毫秒以下按 100 毫秒算
        if (disp == 2) {   // 处置 = 恢复背景:这一帧占的那块清成透明
            for (UINT y = 0; y < fh && top + y < ch; ++y)
                for (UINT x = 0; x < fw && left + x < cw; ++x) std::memset(&canvas[((size_t)(top + y) * cw + (left + x)) * 4], 0, 4);
        } else if (disp == 3 && !saved.empty()) {
            canvas = saved;
        }
    }
    r.frames.swap(frames);
    r.delays.swap(delays);
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

    ComPtr<IWICBitmapSource> src;
    UINT w = 0, h = 0;
    int orient = 1;
    bool custom = false;   // 自己解码出来的(PSD / HDR):已经是预乘 BGRA 的 WIC 位图
    const bool isPsd = (ext == L"psd" || ext == L"psb"), isHdr = (ext == L"hdr"), isExr = (ext == L"exr"), isRaw = isRawExt(ext);
    const bool isSvg = (ext == L"svg" || ext == L"svgz"), isShell = (ext == L"ai" || ext == L"eps" || ext == L"cdr");
    ComPtr<IWICBitmapDecoder> dec;
    bool rawPreview = false;   // 这张 RAW 是不是用的内嵌预览(是:方向以容器里的为准)

    auto useShell = [&]() -> bool {   // 向 Windows 要预览图,成功就当作"自己解码出来的"
        ComPtr<IWICBitmap> sb;
        UINT sw = 0, sh = 0;
        if (!shellThumb(path, sb, sw, sh)) return false;
        sb.As(&src);
        w = sw;
        h = sh;
        r->bits = 32;
        custom = true;
        return true;
    };

    if (isSvg) {
        std::vector<uint8_t> blob;
        if (!readAllBytes(path, blob)) return r;
        if (blob.size() > 2 && blob[0] == 0x1F && blob[1] == 0x8B) {   // .svgz = gzip 压缩的 SVG
            std::vector<uint8_t> raw;
            if (!xcodec::gunzip(blob.data(), blob.size(), raw, (size_t)256 << 20)) { r->err = "无法读取这个 SVGZ(解压失败)"; return r; }
            blob.swap(raw);
        }
        ComPtr<IWICBitmap> sb;
        UINT sw = 0, sh = 0;
        if (!renderSvg(blob, sb, sw, sh)) { r->err = "无法渲染这个 SVG(文件有问题,或系统低于 Windows 10 1703)"; return r; }
        sb.As(&src);
        w = sw;
        h = sh;
        r->bits = 32;
        r->format = "svg";
        custom = true;
    } else if (isShell) {
        if (!useShell()) { r->err = "Windows 没有给这个文件生成预览图(需要安装能打开它的软件,如 Illustrator / CorelDRAW)"; return r; }
    } else if (isPsd || isHdr || isExr) {
        std::vector<uint8_t> blob;
        if (!readAllBytes(path, blob)) return r;
        xcodec::Image im;
        std::string e2;
        if (!(isPsd ? xcodec::decodePsd(blob.data(), blob.size(), im, e2) : isExr ? xcodec::decodeExr(blob.data(), blob.size(), im, e2) : xcodec::decodeHdr(blob.data(), blob.size(), im, e2))) {
            r->err = "无法读取这张图片:" + e2;
            return r;
        }
        blob.clear();
        blob.shrink_to_fit();
        ComPtr<IWICBitmap> mb;
        if (FAILED(g_wic->CreateBitmapFromMemory((UINT)im.w, (UINT)im.h, GUID_WICPixelFormat32bppPBGRA, (UINT)im.w * 4, (UINT)im.bgra.size(), im.bgra.data(), &mb))) return r;
        mb.As(&src);
        w = (UINT)im.w;
        h = (UINT)im.h;
        r->bits = im.bits;
        r->format = im.format;
        custom = true;
    } else if (isRaw) {
        const int mode = g_rawMode.load();   // 0=Auto 1=Preview 2=Develop
        ComPtr<IWICBitmapDecoder> pdec, sdec;
        int porient = 1;
        auto viaPreview = [&]() -> bool {   // 取文件里最大的内嵌 JPEG 预览
            std::vector<uint8_t> blob;
            if (!readAllBytes(path, blob)) return false;
            size_t off = 0, len = 0;
            if (!xcodec::findRawPreview(blob.data(), blob.size(), off, len)) return false;
            porient = xcodec::rawOrientation(blob.data(), blob.size());
            ComPtr<IStream> st;
            st.Attach(SHCreateMemStream(blob.data() + off, (UINT)len));   // 会复制一份,blob 可以释放
            if (!st) return false;
            return SUCCEEDED(g_wic->CreateDecoderFromStream(st.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &pdec)) && pdec;
        };
        auto viaSystem = [&]() -> bool {   // 系统装了 RAW 解码器(Microsoft 原始图像扩展 / 相机厂商编解码器)时走它,得到完整分辨率
            return SUCCEEDED(g_wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &sdec)) && sdec;
        };
        auto bigEnough = [&]() -> bool {   // Auto:预览有 200 万像素以上就够用
            ComPtr<IWICBitmapFrameDecode> f0;
            UINT pw = 0, ph = 0;
            return pdec && SUCCEEDED(pdec->GetFrame(0, &f0)) && SUCCEEDED(f0->GetSize(&pw, &ph)) && (unsigned long long)pw * ph >= 2000000ULL;
        };
        bool havePrev = false;
        if (mode != 2) havePrev = viaPreview();
        if (mode == 2 || (mode == 0 && (!havePrev || !bigEnough()))) {
            if (viaSystem()) { dec = sdec; rawPreview = false; }
        }
        if (!dec) {   // 系统没有 RAW 解码器(或模式要求只用预览):用内嵌预览
            if (!havePrev) havePrev = viaPreview();
            if (!havePrev) { r->err = "这个 RAW 文件里没有找到内嵌的预览图,系统里也没有能读它的 RAW 解码器"; return r; }
            dec = pdec;
            orient = porient;
            rawPreview = true;
        }
    } else {
        if (FAILED(g_wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) {
            if (!useShell()) return r;   // 系统解码器不认:最后问问 Windows 有没有预览图
        }
    }

    if (!custom) {
        GUID cf;
        bool isIco = false;
        if (SUCCEEDED(dec->GetContainerFormat(&cf)) && !rawPreview) {
            if (cf == GUID_ContainerFormatJpeg) r->format = "jpeg";
            else if (cf == GUID_ContainerFormatPng) r->format = "png";
            else if (cf == GUID_ContainerFormatGif) r->format = "gif";
            else if (cf == GUID_ContainerFormatBmp) r->format = "bmp";
            else if (cf == GUID_ContainerFormatTiff) r->format = "tiff";
            else if (cf == GUID_ContainerFormatIco) { r->format = "ico"; isIco = true; }
        }
        bool gifOk = false;
        if (cf == GUID_ContainerFormatGif && !g_gifOff.load()) {
            UINT gw = 0, gh = 0;
            if (decodeGifFrames(dec.Get(), *r, gw, gh)) {   // 动画:第一帧当静态图用,其余帧留在 r->frames 里
                ComPtr<IWICBitmap> f0 = r->frames[0];
                f0.As(&src);
                w = gw;
                h = gh;
                r->bits = 8;
                gifOk = true;
            }
        }
        if (!gifOk) {
        UINT fidx = 0;
        if (isIco) {   // 图标里常有 16~256 多个尺寸:取最大的那张
            UINT fc = 0;
            if (SUCCEEDED(dec->GetFrameCount(&fc))) {
                unsigned long long best = 0;
                for (UINT k = 0; k < fc; ++k) {
                    ComPtr<IWICBitmapFrameDecode> f2;
                    UINT fw = 0, fh = 0;
                    if (SUCCEEDED(dec->GetFrame(k, &f2)) && SUCCEEDED(f2->GetSize(&fw, &fh)) && (unsigned long long)fw * fh > best) { best = (unsigned long long)fw * fh; fidx = k; }
                }
            }
        }
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(dec->GetFrame(fidx, &frame))) return r;
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
        int fo = 1;
        if (queryOrientation(frame.Get(), fo) && !(rawPreview && orient != 1)) orient = fo;   // 内嵌预览的 RAW 以容器里的方向为准
        frame.As(&src);
        }   // !gifOk
    }

    // 先缩到不超过 16384(再大的图显卡贴不上),再转成预乘 BGRA
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
    {
        const double gm = g_gamma.load();
        if (gm > 0.0) {   // Display/GammaEnabled
            if (auto nb = applyGamma(bmp.Get(), gm)) bmp = nb;
            for (auto& fr : r->frames) if (auto nf = applyGamma(fr.Get(), gm)) fr = nf;
        }
    }
    r->bmp = bmp;
    r->texW = tw;
    r->texH = th;
    // 原图尺寸:被缩过就按比例还原(转 90 度时宽高对调)
    bool swap = orient >= 5;
    UINT ow = swap ? h : w, oh = swap ? w : h;
    r->origW = ow;
    r->origH = oh;
    r->bytes = (size_t)tw * th * 4 * (1 + (r->frames.size() > 1 ? r->frames.size() : 0));
    if (r->frames.size() < 2) { r->frames.clear(); r->delays.clear(); }
    r->ok = true;
    r->err.clear();
    r->secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

// ------------------------------------------------------------------ 缓存(预读也放这里)
class Cache {
public:
    explicit Cache(size_t limitBytes) : limit_(limitBytes) {}
    void setMaxEntries(size_t n) { std::lock_guard<std::mutex> g(m_); maxEntries_ = std::max<size_t>(2, n); }
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
        while ((bytes_ > limit_ || map_.size() > maxEntries_) && lru_.size() > 2) {   // 至少留 2 张(当前 + 下一张)
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
    size_t maxEntries_ = 64;
};

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
    unsigned bg = 0xE0E0E0;   // 主题背景色:Display/DarkTitleBar=Auto 时按它的明暗决定标题栏深浅
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
    "NextImage", "PrevImage", "FirstImage", "LastImage", "PrevFolder", "NextFolder",
    "ZoomIn", "ZoomOut", "FitWindow", "ActualSize", "ZoomLock",
    "RotateLeft", "RotateRight", "RotateBy", "Fullscreen", "AlwaysOnTop",
    "ReloadImage", "OpenFile", "SaveAs", "CopyImage", "CopyPath", "ShowInExplorer", "Properties", "Delete",
    "CopyTo", "MoveTo", "Rename", "Undo", "SetWallpaper", "Print",
    "ToggleToolbar", "ToggleStatusBar", "ToggleMenu", "ToggleKeyHints", "ToggleFilmstrip", "ToggleSlideshow",
    "About",
    // 下面这些要用 Xdir(评分/色标/收藏存在 SQLite 里,选项窗口是 Qt 写的):原生版把命令交给 Xdir 去办
    "Options", "ManageExtensions", "FileAssoc", "FileUnassoc", "CommandPalette",
    "Rate0", "Rate1", "Rate2", "Rate3", "Rate4", "Rate5", "LabelRed", "LabelYellow", "LabelGreen", "LabelBlue",
    "ToggleFavorite", "ShowFavorites",
    "ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown"};

bool implemented(const std::string& c) {
    for (auto k : kCmds) if (c == k) return true;
    return false;
}

struct Icon {
    SvgIcon svg;
    std::vector<ComPtr<ID2D1PathGeometry>> geos;
    ComPtr<ID2D1Bitmap> bmp;   // 用户自己的 png/ico/jpg 图标:彩色原样缩放,不上色
    bool raster = false;
    bool ok = false;
};

struct Button {
    std::string cmd;
    bool sep = false;
    RECT r{};
};

// ------------------------------------------------------------------ 应用
// ---------------- 简单输入框(模态)
struct PromptState { std::wstring title, label, text; int selLen = -1; bool ok = false, done = false; HWND edit = nullptr; HFONT font = nullptr; };
LRESULT CALLBACK PromptProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    PromptState* ps = (PromptState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_CREATE: {
        ps = (PromptState*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)ps);
        NONCLIENTMETRICSW ncm;
        ZeroMemory(&ncm, sizeof ncm);
        ncm.cbSize = sizeof ncm;
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
        ps->font = CreateFontIndirectW(&ncm.lfMessageFont);
        HINSTANCE hi = GetModuleHandleW(nullptr);
        HWND lab = CreateWindowExW(0, L"STATIC", ps->label.c_str(), WS_CHILD | WS_VISIBLE, 12, 12, 396, 20, h, nullptr, hi, nullptr);
        ps->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ps->text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 12, 36, 396, 24, h, (HMENU)(INT_PTR)100, hi, nullptr);
        HWND ok = CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 238, 74, 80, 26, h, (HMENU)(INT_PTR)IDOK, hi, nullptr);
        HWND cn = CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 328, 74, 80, 26, h, (HMENU)(INT_PTR)IDCANCEL, hi, nullptr);
        for (HWND c : {lab, ps->edit, ok, cn}) SendMessageW(c, WM_SETFONT, (WPARAM)ps->font, TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK && ps) {
            wchar_t buf[1024];
            GetWindowTextW(ps->edit, buf, 1024);
            ps->text = buf;
            ps->ok = true;
            DestroyWindow(h);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) { DestroyWindow(h); return 0; }
        break;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY:
        if (ps) { ps->done = true; if (ps->font) { DeleteObject(ps->font); ps->font = nullptr; } }
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}
bool promptText(HWND owner, const std::wstring& title, const std::wstring& label, std::wstring& text, int selLen = -1) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof wc);
        wc.lpfnWndProc = PromptProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"XviewPrompt";
        RegisterClassW(&wc);
        reg = true;
    }
    PromptState ps;
    ps.title = title; ps.label = label; ps.text = text; ps.selLen = selLen;
    RECT rc = {0, 0, 420, 112};
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectEx(&rc, style, FALSE, WS_EX_DLGMODALFRAME);
    RECT orc = {0, 0, 0, 0};
    if (owner) GetWindowRect(owner, &orc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int x = owner ? orc.left + ((orc.right - orc.left) - w) / 2 : CW_USEDEFAULT;
    int y = owner ? orc.top + ((orc.bottom - orc.top) - h) / 2 : CW_USEDEFAULT;
    HWND d = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"XviewPrompt", title.c_str(), style, x, y, w, h, owner, nullptr, GetModuleHandleW(nullptr), &ps);
    if (!d) return false;
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(d, SW_SHOW);
    SetForegroundWindow(d);
    SetFocus(ps.edit);
    int len = (int)ps.text.size();
    SendMessageW(ps.edit, EM_SETSEL, 0, selLen >= 0 ? selLen : len);
    MSG msg;
    while (!ps.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(d, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (owner) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
    if (ps.ok) text = ps.text;
    return ps.ok;
}

// 把一幅 WIC 图写成文件(扩展名决定格式)
// 把位图按当前旋转角度转出来(90 度倍数精确转,其它角度双线性插值,空白处透明)。角度为 0 时原样返回。
ComPtr<IWICBitmapSource> rotatedForSave(IWICBitmap* bmp, UINT w, UINT h, double deg, UINT& ow, UINT& oh) {
    ow = w; oh = h;
    ComPtr<IWICBitmapSource> same(bmp);
    double d = std::fmod(std::fmod(deg, 360.0) + 360.0, 360.0);
    if (!bmp || !g_wic || d < 1e-6 || d > 360.0 - 1e-6) return same;
    std::vector<BYTE> px((size_t)w * h * 4);
    WICRect rc = {0, 0, (INT)w, (INT)h};
    if (FAILED(bmp->CopyPixels(&rc, w * 4, (UINT)px.size(), px.data()))) return same;
    const double a = d * 3.14159265358979323846 / 180.0, c = std::cos(a), sn = std::sin(a);
    double bw = std::fabs(w * c) + std::fabs(h * sn), bh = std::fabs(w * sn) + std::fabs(h * c);
    if (std::fabs(d - 90.0) < 1e-6 || std::fabs(d - 270.0) < 1e-6) { bw = h; bh = w; }
    else if (std::fabs(d - 180.0) < 1e-6) { bw = w; bh = h; }
    UINT W = (UINT)std::max(1.0, std::floor(bw + 0.5)), H = (UINT)std::max(1.0, std::floor(bh + 0.5));
    if ((size_t)W * H > (size_t)20000 * 20000) return same;
    std::vector<BYTE> out((size_t)W * H * 4, 0);
    const double cx = w / 2.0, cy = h / 2.0, ox = W / 2.0, oy = H / 2.0;
    for (UINT y = 0; y < H; ++y) {
        for (UINT x = 0; x < W; ++x) {
            double dx = x + 0.5 - ox, dy = y + 0.5 - oy;
            double sx = dx * c + dy * sn + cx - 0.5, sy = -dx * sn + dy * c + cy - 0.5;   // 逆向映射回原图
            BYTE* o = &out[((size_t)y * W + x) * 4];
            if (sx < -0.5 || sy < -0.5 || sx > w - 0.5 || sy > h - 0.5) continue;
            int x0 = (int)std::floor(sx), y0 = (int)std::floor(sy);
            double fx = sx - x0, fy = sy - y0;
            for (int k = 0; k < 4; ++k) {
                double v = 0;
                for (int j = 0; j < 2; ++j)
                    for (int i = 0; i < 2; ++i) {
                        int xx = std::min<int>((int)w - 1, std::max(0, x0 + i)), yy = std::min<int>((int)h - 1, std::max(0, y0 + j));
                        v += px[((size_t)yy * w + xx) * 4 + k] * (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                    }
                o[k] = (BYTE)std::min(255.0, v + 0.5);
            }
        }
    }
    ComPtr<IWICBitmap> nb;
    if (FAILED(g_wic->CreateBitmapFromMemory(W, H, GUID_WICPixelFormat32bppPBGRA, W * 4, (UINT)out.size(), out.data(), &nb))) return same;
    ow = W; oh = H;
    ComPtr<IWICBitmapSource> r;
    nb.As(&r);
    return r;
}

bool saveBitmapFile(IWICBitmapSource* src, UINT w, UINT h, const std::wstring& path) {
    if (!src || !g_wic) return false;
    std::wstring e = extOf(path);
    GUID fmt = GUID_ContainerFormatPng;
    bool opaque = false;
    if (e == L"jpg" || e == L"jpeg") { fmt = GUID_ContainerFormatJpeg; opaque = true; }
    else if (e == L"bmp") { fmt = GUID_ContainerFormatBmp; opaque = true; }
    else if (e == L"tif" || e == L"tiff") fmt = GUID_ContainerFormatTiff;
    else if (e == L"gif") fmt = GUID_ContainerFormatGif;
    ComPtr<IWICStream> st;
    if (FAILED(g_wic->CreateStream(&st))) return false;
    if (FAILED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(g_wic->CreateEncoder(fmt, nullptr, &enc))) return false;
    if (FAILED(enc->Initialize(st.Get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> fr;
    ComPtr<IPropertyBag2> pb;
    if (FAILED(enc->CreateNewFrame(&fr, &pb))) return false;
    if (e == L"jpg" || e == L"jpeg") {
        PROPBAG2 opt;
        ZeroMemory(&opt, sizeof opt);
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.92f;
        pb->Write(1, &opt, &v);
    }
    if (FAILED(fr->Initialize(pb.Get()))) return false;
    if (FAILED(fr->SetSize(w, h))) return false;
    WICPixelFormatGUID pf = opaque ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
    if (FAILED(fr->SetPixelFormat(&pf))) return false;
    ComPtr<IWICFormatConverter> cv;
    if (FAILED(g_wic->CreateFormatConverter(&cv))) return false;
    if (FAILED(cv->Initialize(src, pf, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;
    if (FAILED(fr->WriteSource(cv.Get(), nullptr))) return false;
    if (FAILED(fr->Commit())) return false;
    return SUCCEEDED(enc->Commit());
}


bool sameFileContent(const std::wstring& a, const std::wstring& b) {
    WIN32_FILE_ATTRIBUTE_DATA da, db;
    if (!GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &da) || !GetFileAttributesExW(b.c_str(), GetFileExInfoStandard, &db)) return false;
    if (da.nFileSizeHigh != db.nFileSizeHigh || da.nFileSizeLow != db.nFileSizeLow) return false;
    FILE* fa = _wfopen(a.c_str(), L"rb");
    FILE* fb = _wfopen(b.c_str(), L"rb");
    bool same = fa && fb;
    if (same) {
        std::vector<char> x(1 << 16), y(1 << 16);
        for (;;) {
            size_t n = fread(x.data(), 1, x.size(), fa), m = fread(y.data(), 1, y.size(), fb);
            if (n != m || memcmp(x.data(), y.data(), n) != 0) { same = false; break; }
            if (n == 0) break;
        }
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return same;
}
std::wstring uniqueName(const std::wstring& dir, const std::wstring& name, const std::wstring& sep) {
    size_t dot = name.find_last_of(L'.');
    std::wstring stem = (dot == std::wstring::npos || dot == 0) ? name : name.substr(0, dot);
    std::wstring ext = (dot == std::wstring::npos || dot == 0) ? L"" : name.substr(dot);
    for (int n = 1; n < 10000; ++n) {
        std::wstring c = dir + L"\\" + stem + sep + std::to_wstring(n) + ext;
        if (GetFileAttributesW(c.c_str()) == INVALID_FILE_ATTRIBUTES) return c;
    }
    return dir + L"\\" + name;
}


struct App {
    HWND hwnd = nullptr;
    std::wstring root;
    std::string dblPending;   // 双击要执行的命令,等鼠标松开
    Ini cfg, loc, nat;
    Theme th;
    std::map<std::string, std::string> iconOf;
    Keymap keys;
    std::set<std::wstring> exts;
    Cache cache{512ull << 20};
    Pool pool;
    Pool thumbPool;
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
    double rotDeg = 0;   // 顺时针旋转角度(右/左转 = ±90,"旋转..."可以输入任意角度)
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

    // ---- 新增状态
    HMENU menubar = nullptr;
    std::vector<std::string> menuCmd;                       // 菜单栏里 ID_MENU_BASE+i 对应的命令
    bool showMenu = false, keyHints = false, showFilm = false, slideshow = false;
    std::string hintText;                                   // 状态栏左侧的按键提示
    std::map<std::string, std::pair<std::string, std::string>> custom;   // buttons.json:命令 → (图标文件, 提示)
    HWND tip = nullptr;                                     // 按钮提示气泡
    std::string tipSig;
    std::deque<std::wstring> tipTexts;
    int tipCount = 0;
    size_t animIdx = 0;                                     // GIF 当前帧
    std::vector<ComPtr<ID2D1Bitmap>> animBmps;
    struct UndoOp { int kind; std::wstring a, b; };         // kind 1:b 改名/移动成了 a 之前的样子(撤销 = 把 b 搬回 a);kind 2:a 被删进回收站
    std::vector<UndoOp> undoStack;
    std::map<std::wstring, ComPtr<ID2D1Bitmap>> thumbs;     // 胶片条缩略图
    std::set<std::wstring> thumbFail;
    bool thumbMore = false;
    int darkApplied = -1;
    unsigned long long cfgStamp = 0;                        // data\config\settings.ini 的修改时间:变了就重读(在 Xdir 里改了选项)

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
        r.bottom = std::max(r.top, r.bottom - stH() - filmH());
        return r;
    }
    int filmH() const {
        if (!showFilm || fullscreen || files.empty()) return 0;
        return S(std::max(40, std::min(200, cfg.getInt("Viewer/FilmstripHeight", 84))));
    }
    RECT filmRect() const {
        RECT r = client();
        r.bottom = std::max<LONG>(0, r.bottom - stH());
        r.top = std::max<LONG>(0, r.bottom - filmH());
        return r;
    }
    bool hasImage() const { return cur && cur->ok && bitmap; }
    // 旋转后能装下整幅图的外框尺寸(90 度的整数倍时就是宽高互换)
    void rotBounds(double& bw, double& bh) const {
        double w = cur->origW, h = cur->origH;
        double r = std::fmod(std::fmod(rotDeg, 180.0) + 180.0, 180.0);
        if (r == 0.0) { bw = w; bh = h; return; }
        if (r == 90.0) { bw = h; bh = w; return; }
        double a = rotDeg * 3.14159265358979323846 / 180.0, c = std::fabs(std::cos(a)), sn = std::fabs(std::sin(a));
        bw = w * c + h * sn;
        bh = w * sn + h * c;
    }
    double effW() const { double a, b; rotBounds(a, b); return a; }
    double effH() const { double a, b; rotBounds(a, b); return b; }
    void invalidate() { if (hwnd) InvalidateRect(hwnd, nullptr, FALSE); }

    // ---------------- 配置加载
    void loadConfig() {
        th = Theme();
        exts.clear();
        iconOf.clear();
        keys = Keymap();
        icons.clear();
        custom.clear();
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

        iconOf["@Menu"] = iconOf["ToggleMenu"];
        // 用户给按钮换的图标 / 写的提示(data\config\buttons.json:{命令ID:{icon,tip}})
        Json bj;
        if (parseJson(readFileText(root + L"\\data\\config\\buttons.json"), bj) && bj.t == Json::Obj)
            for (auto& p : bj.o) {
                if (p.second.t != Json::Obj) continue;
                std::string ic = p.second.str("icon"), tp = p.second.str("tip");
                custom[p.first] = {ic, tp};
                if (!ic.empty()) iconOf[p.first] = ic;
            }
        // 主题
        std::string tname = trim(readFileText(root + L"\\data\\config\\theme.txt"));
        if (tname.compare(0, 3, "\xEF\xBB\xBF") == 0) tname.erase(0, 3);
        if (!tname.empty()) {
            Json tj;
            if (parseJson(readFileText(root + L"\\app\\config\\themes\\" + W(tname) + L".json"), tj) && tj.t == Json::Obj) {
                auto col = [&](const char* k, unsigned& dst) { dst = parseHexColor(tj.str(k), dst); };
                col("toolbar_bg", th.toolbarBg); col("status_bg", th.statusBg); col("status_fg", th.statusFg);
                col("icon", th.icon); col("canvas", th.canvas); col("canvas_error", th.canvasErr);
                col("hover", th.hover); col("border", th.border); col("accent_soft", th.accentSoft); col("bg", th.bg);
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
        if (exts.empty()) for (auto e : {L"jpg", L"jpeg", L"png", L"gif", L"bmp", L"webp", L"tif", L"tiff", L"ico", L"jfif", L"psd", L"psb", L"hdr", L"exr", L"svg", L"svgz"}) exts.insert(e);
        // 快捷键:只收录原生版实现了的命令
        for (auto c : kCmds) keys.add(c, cfg.getList(std::string("Keys/") + c));
        showToolbar = cfg.getBool("Viewer/ShowToolbar", true);
        showStatus = cfg.getBool("Viewer/ShowStatusBar", true);
        zoomLock = cfg.getBool("Viewer/ZoomLock", false);
        topmost = cfg.getBool("Viewer/AlwaysOnTop", false);
        showMenu = cfg.getBool("Viewer/ShowMenu", false);
        keyHints = cfg.getBool("Viewer/ShowKeyHints", false);
        showFilm = cfg.getBool("Viewer/ShowFilmstrip", false);
        { std::string rm = cfg.get("Viewer/RawMode", "Auto"); g_rawMode = rm == "Preview" ? 1 : rm == "Develop" ? 2 : 0; }
        g_gamma = cfg.getBool("Display/GammaEnabled", false) ? std::max(0.1, cfg.getDouble("Display/Gamma", 1.0)) : 0.0;
        g_gifOff = cfg.getBool("Display/DisableGifAnimation", false);
        cache.setMaxEntries((size_t)std::max(cfg.getInt("Viewer/CacheCount", 5), 2 * std::min(5, std::max(0, cfg.getInt("Viewer/PreloadCount", 1))) + 1));
        hintText.clear();   // 状态栏左侧的按键提示:常用命令的第一个快捷键
        for (auto c : {"NextImage", "PrevImage", "ZoomIn", "ZoomOut", "FitWindow", "ActualSize", "RotateLeft", "RotateRight", "Fullscreen", "BackToBrowser"}) {
            auto k = cfg.getList(std::string("Keys/") + c);
            if (k.empty()) continue;
            if (!hintText.empty()) hintText += "    ";
            hintText += k[0] + " " + cmdLabel(c);
        }
        cfgStamp = settingsStamp();
    }
    unsigned long long settingsStamp() const {   // 用户设置文件的修改时间(0 = 没有这个文件)
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (!GetFileAttributesExW((root + L"\\data\\config\\settings.ini").c_str(), GetFileExInfoStandard, &fad)) return 0;
        return ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
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
        animBmps.clear();
        thumbs.clear();
        for (auto it = icons.begin(); it != icons.end();) { if (it->second.raster) it = icons.erase(it); else ++it; }
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
        std::wstring p1 = root + L"\\data\\config\\icons\\" + W(file), p2 = root + L"\\app\\resources\\icons\\" + W(file);
        if (extOf(W(file)) != L"svg") {   // png / ico / jpg:用户自己的彩色图标
            ic.raster = true;
            std::wstring pp = fileExists(p1) ? p1 : p2;
            ComPtr<IWICBitmapDecoder> dc;
            ComPtr<IWICBitmapFrameDecode> fr;
            ComPtr<IWICFormatConverter> cv;
            if (rt && SUCCEEDED(g_wic->CreateDecoderFromFilename(pp.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dc)) &&
                SUCCEEDED(dc->GetFrame(0, &fr)) && SUCCEEDED(g_wic->CreateFormatConverter(&cv)) &&
                SUCCEEDED(cv->Initialize(fr.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut)))
                rt->CreateBitmapFromWicBitmap(cv.Get(), nullptr, &ic.bmp);
            ic.ok = ic.bmp != nullptr;
            if (!ic.ok) icons.erase(file);
            return ic.ok ? &icons[file] : nullptr;
        }
        std::string txt = readFileText(p1);
        if (txt.empty()) txt = readFileText(p2);
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
        if (c == "ToggleMenu") return showMenu;
        if (c == "ToggleKeyHints") return keyHints;
        if (c == "ToggleFilmstrip") return showFilm;
        if (c == "ToggleSlideshow") return slideshow;
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
                if (tok == "@Menu") { if (showMenu) continue; }   // 菜单栏已经显示时,这个按钮就多余了
                else if (tok[0] == '@' || !implemented(tok)) continue;
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
        if (cmd == "@Menu") return cmdLabel("ToggleMenu");
        auto cu = custom.find(cmd);
        std::string s = (cu != custom.end() && !cu->second.second.empty()) ? cu->second.second : cmdLabel(cmd);   // 用户自己写的提示优先
        auto k = cfg.getList("Keys/" + cmd);
        if (!k.empty()) s += "  (" + k[0] + ")";
        return s;
    }

    // ---------------- 菜单栏 / 弹出菜单(设置里的 [MenuViewer])
    void appendCmd(HMENU m, const std::string& tok, std::vector<std::string>& list, UINT base) {
        std::wstring label = W(cmdLabel(tok));
        auto k = cfg.getList("Keys/" + tok);
        if (!k.empty()) label += L"\t" + W(k[0]);
        UINT id = base + (UINT)list.size();
        list.push_back(tok);
        AppendMenuW(m, MF_STRING | (checked(tok) ? MF_CHECKED : 0), id, label.c_str());
    }
    HMENU makeMenuTree(bool popup, std::vector<std::string>& list, UINT base) {
        HMENU root_ = popup ? CreatePopupMenu() : CreateMenu();
        for (auto& name : cfg.getList("MenuViewer/Order")) {
            HMENU sub = CreatePopupMenu();
            int n = 0;
            bool lastSep = true;
            for (auto& tok : cfg.getList("MenuViewer/" + name)) {
                if (tok == "|") { if (!lastSep) { AppendMenuW(sub, MF_SEPARATOR, 0, nullptr); lastSep = true; } continue; }
                if (!implemented(tok) || tok.compare(0, 5, "Arrow") == 0) continue;
                appendCmd(sub, tok, list, base);
                lastSep = false;
                ++n;
            }
            if (n && lastSep) RemoveMenu(sub, (UINT)GetMenuItemCount(sub) - 1, MF_BYPOSITION);
            if (n) AppendMenuW(root_, MF_POPUP, (UINT_PTR)sub, W(tr("menu." + name, name)).c_str());
            else DestroyMenu(sub);
        }
        return root_;
    }
    void buildMenus() {
        if (menubar) {
            if (hwnd && GetMenu(hwnd) == menubar) SetMenu(hwnd, nullptr);
            DestroyMenu(menubar);
            menubar = nullptr;
        }
        menuCmd.clear();
        menubar = makeMenuTree(false, menuCmd, ID_MENU_BASE);
    }
    void applyMenu() {   // ShowMenu / ToggleMenu;全屏时一律不显示
        if (!hwnd) return;
        bool want = showMenu && !fullscreen;
        if (want) {
            if (!menubar) buildMenus();
            if (GetMenu(hwnd) != menubar) SetMenu(hwnd, menubar);
        } else if (GetMenu(hwnd)) {
            SetMenu(hwnd, nullptr);
        }
        DrawMenuBar(hwnd);
    }
    void refreshMenuChecks(HMENU h) {
        for (int i = 0, n = GetMenuItemCount(h); i < n; ++i) {
            UINT id = GetMenuItemID(h, i);
            if (id >= ID_MENU_BASE && id < ID_MENU_BASE + menuCmd.size())
                CheckMenuItem(h, (UINT)i, MF_BYPOSITION | (checked(menuCmd[id - ID_MENU_BASE]) ? MF_CHECKED : MF_UNCHECKED));
        }
    }
    void popupAllMenus(int sx, int sy) {   // 工具栏上的 @Menu 按钮:把所有菜单合在一起弹出
        std::vector<std::string> list;
        HMENU m = makeMenuTree(true, list, ID_MENU_BASE + 1000);
        SetForegroundWindow(hwnd);
        int sel = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTBUTTON, sx, sy, 0, hwnd, nullptr);
        DestroyMenu(m);
        if (sel >= (int)(ID_MENU_BASE + 1000) && sel - (int)(ID_MENU_BASE + 1000) < (int)list.size()) run(list[(size_t)(sel - (int)(ID_MENU_BASE + 1000))]);
    }

    // ---------------- 按钮提示气泡(Misc/ShowToolTips)
    void syncTips() {
        if (!hwnd) return;
        if (!cfg.getBool("Misc/ShowToolTips", true)) {
            if (tip) { DestroyWindow(tip); tip = nullptr; tipSig.clear(); tipCount = 0; tipTexts.clear(); }
            return;
        }
        if (!tip) {
            INITCOMMONCONTROLSEX ic = {sizeof ic, ICC_BAR_CLASSES};
            InitCommonControlsEx(&ic);
            tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                  CW_USEDEFAULT, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!tip) return;
            tipSig.clear();
        }
        std::string sig;
        for (auto& b : buttons) if (!b.sep) sig += b.cmd + ":" + std::to_string(b.r.left) + "," + std::to_string(b.r.top) + "," + std::to_string(b.r.right) + "," + std::to_string(b.r.bottom) + ";";
        if (sig == tipSig) return;
        tipSig = sig;
        for (int i = 1; i <= tipCount; ++i) {
            TOOLINFOW ti;
            ZeroMemory(&ti, sizeof ti);
            ti.cbSize = sizeof ti;
            ti.hwnd = hwnd;
            ti.uId = (UINT_PTR)i;
            SendMessageW(tip, TTM_DELTOOLW, 0, (LPARAM)&ti);
        }
        tipCount = 0;
        tipTexts.clear();
        for (auto& b : buttons) {
            if (b.sep) continue;
            tipTexts.push_back(W(hintFor(b.cmd)));
            TOOLINFOW ti;
            ZeroMemory(&ti, sizeof ti);
            ti.cbSize = sizeof ti;
            ti.uFlags = TTF_SUBCLASS;
            ti.hwnd = hwnd;
            ti.uId = (UINT_PTR)(++tipCount);
            ti.rect = b.r;
            ti.lpszText = const_cast<LPWSTR>(tipTexts.back().c_str());
            SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
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
        if (ic.raster) {
            if (ic.bmp) rt->DrawBitmap(ic.bmp.Get(), D2D1::RectF(x, y, x + size, y + size), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            return;
        }
        float s = size / (float)ic.svg.vbW;
        rt->SetTransform(D2D1::Matrix3x2F::Translation((float)-ic.svg.vbX, (float)-ic.svg.vbY) * D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(x, y));
        setBrush((!cfg.getBool("Icons/Tint", true) && ic.svg.hasStroke) ? ic.svg.stroke : th.icon);   // Tint=0:图标文件自带的颜色
        for (auto& g : ic.geos) rt->DrawGeometry(g.Get(), brush.Get(), (float)ic.svg.strokeWidth, stroke.Get());
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
    }
    void drawImage() {
        RECT c = canvasRect();
        float vw = (float)(c.right - c.left), vh = (float)(c.bottom - c.top);
        float cx = c.left + vw / 2 + (float)offX, cy = c.top + vh / 2 + (float)offY;
        float dw = (float)(cur->origW * zoom), dh = (float)(cur->origH * zoom);
        rt->PushAxisAlignedClip(D2D1::RectF((float)c.left, (float)c.top, (float)c.right, (float)c.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        rt->SetTransform(D2D1::Matrix3x2F::Rotation((float)rotDeg, D2D1::Point2F(cx, cy)));
        rt->DrawBitmap(frameBitmap(), D2D1::RectF(cx - dw / 2, cy - dh / 2, cx + dw / 2, cy + dh / 2), 1.0f,
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
        else if (keyHints) left = hintText;
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
        syncTips();
        applyTitleBar();
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
        drawFilm();
        drawToolbar();
        drawStatus();
        HRESULT hr = rt->EndDraw();
        if (thumbMore) { thumbMore = false; PostMessageW(hwnd, WM_APP_THUMB, 0, 0); }
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
        rotateBy(dir > 0 ? 90.0 : -90.0);
    }
    void rotateBy(double deg) {
        if (!hasImage()) return;
        rotDeg = std::fmod(rotDeg + deg, 360.0);
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
        cur = l;
        bitmap.Reset();
        runtimeErr.clear();
        rotDeg = 0;
        offX = offY = 0;
        animIdx = 0;
        animBmps.clear();
        if (l->ok && rt) ensureBitmap();
        startAnim();
        resizeToImage();
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
        present(c);   // 读不了的图:画面上显示错误提示(不会交给别的程序)
    }

    void updateTitle() {
        std::string name = "Xview";
        if (idx >= 0 && idx < (int)files.size()) {
            std::wstring p = cfg.getBool("Viewer/ShowFullPath", true) ? files[idx] : baseName(files[idx]);
            name = fmtArgs(tr("app.titleFormat", "%1 - Xview"), U(p));
        }
        if (cfg.getBool("Misc/TitleShowVersion", false)) name += " v1.0";
        SetWindowTextW(hwnd, W(name).c_str());
        reportState(false);
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
        pushUndo(2, p, L"");
        if (cfg.getBool("Viewer/ExitAfterDelete", false)) { closeWindow(); return; }
        cache.erase(p);
        files.erase(files.begin() + idx);
        cur.reset();
        bitmap.Reset();
        animBmps.clear();
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

    // ---------------- GIF 动画
    ID2D1Bitmap* frameBitmap() {
        if (cur && cur->frames.size() > 1 && !g_gifOff.load()) {
            size_t n = cur->frames.size();
            if (animBmps.size() != n) {
                animBmps.assign(n, ComPtr<ID2D1Bitmap>());
                for (size_t i = 0; i < n; ++i) rt->CreateBitmapFromWicBitmap(cur->frames[i].Get(), animBmps[i].ReleaseAndGetAddressOf());
            }
            ID2D1Bitmap* b = animBmps[animIdx % n].Get();
            if (b) return b;
        }
        return bitmap.Get();
    }
    void startAnim() {
        KillTimer(hwnd, TIMER_ANIM);
        animIdx = 0;
        if (cur && cur->ok && cur->frames.size() > 1 && !g_gifOff.load() && !cur->delays.empty())
            SetTimer(hwnd, TIMER_ANIM, (UINT)std::max(20, cur->delays[0]), nullptr);
    }
    void stepAnim() {
        KillTimer(hwnd, TIMER_ANIM);
        if (!cur || cur->frames.size() < 2 || g_gifOff.load()) return;
        animIdx = (animIdx + 1) % cur->frames.size();
        invalidate();
        int d = animIdx < cur->delays.size() ? cur->delays[animIdx] : 100;
        SetTimer(hwnd, TIMER_ANIM, (UINT)std::max(20, d), nullptr);
    }

    // ---------------- 幻灯片放映
    void toggleSlideshow() {
        if (files.empty()) return;
        slideshow = !slideshow;
        if (slideshow) SetTimer(hwnd, TIMER_SHOW, (UINT)(std::max(1, cfg.getInt("Viewer/SlideshowSeconds", 3)) * 1000), nullptr);
        else KillTimer(hwnd, TIMER_SHOW);
        invalidate();
    }
    void slideStep() {
        int cnt = (int)files.size();
        if (!slideshow || cnt == 0) { KillTimer(hwnd, TIMER_SHOW); return; }
        int n = idx + 1;
        if (n >= cnt) {
            if (!cfg.getBool("Viewer/SlideshowLoop", true)) { slideshow = false; KillTimer(hwnd, TIMER_SHOW); invalidate(); return; }
            n = 0;
        }
        go(n);
    }

    // ---------------- 胶片条
    void filmLayout(int& first, int& visible, int& cw) const {
        RECT fr = filmRect();
        int cell = std::max(8, filmH() - S(8)), pad = S(4);
        cw = cell + pad;
        int n = (int)files.size();
        visible = std::max(1, (int)((fr.right - fr.left - pad) / cw));
        first = idx - visible / 2;
        first = std::max(0, std::min(first, std::max(0, n - visible)));
    }
    ID2D1Bitmap* thumbFor(const std::wstring& path, int size, int& made) {
        auto it = thumbs.find(path);
        if (it != thumbs.end()) return it->second.Get();
        if (thumbFail.count(path)) return nullptr;
        if (made >= 2) { thumbMore = true; return nullptr; }
        ++made;
        if (thumbs.size() > 400) thumbs.clear();
        ComPtr<IShellItemImageFactory> f;
        bool ok = false;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f)))) {
            SIZE sz = {size, size};
            HBITMAP hb = nullptr;
            if (SUCCEEDED(f->GetImage(sz, SIIGBF_BIGGERSIZEOK, &hb)) && hb) {
                ComPtr<IWICBitmap> wb;
                if (SUCCEEDED(g_wic->CreateBitmapFromHBITMAP(hb, nullptr, WICBitmapUseAlpha, &wb))) {
                    ComPtr<IWICFormatConverter> cv;
                    ComPtr<ID2D1Bitmap> db;
                    if (SUCCEEDED(g_wic->CreateFormatConverter(&cv)) &&
                        SUCCEEDED(cv->Initialize(wb.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
                        SUCCEEDED(rt->CreateBitmapFromWicBitmap(cv.Get(), &db)) && db) {
                        thumbs[path] = db;
                        ok = true;
                    }
                }
                DeleteObject(hb);
            }
        }
        if (!ok) { thumbFail.insert(path); return nullptr; }
        return thumbs[path].Get();
    }
    void drawFilm() {
        int h = filmH();
        if (!h) return;
        RECT fr = filmRect();
        fillRect(fr, th.statusBg);
        int first, visible, cw;
        filmLayout(first, visible, cw);
        int cell = cw - S(4), pad = S(4), made = 0, n = (int)files.size();
        for (int i = 0; i < visible && first + i < n; ++i) {
            int k = first + i;
            int x = fr.left + pad + i * cw;
            RECT cr = {x, fr.top + S(4), x + cell, fr.top + S(4) + cell};
            if (k == idx) fillRect({cr.left - S(2), cr.top - S(2), cr.right + S(2), cr.bottom + S(2)}, th.accentSoft);
            ID2D1Bitmap* tb = thumbFor(files[k], cell, made);
            if (!tb) continue;
            D2D1_SIZE_F bs = tb->GetSize();
            float sc = std::min((float)cell / bs.width, (float)cell / bs.height);
            float dw = bs.width * sc, dh = bs.height * sc;
            float cx = (cr.left + cr.right) / 2.0f, cy = (cr.top + cr.bottom) / 2.0f;
            rt->DrawBitmap(tb, D2D1::RectF(cx - dw / 2, cy - dh / 2, cx + dw / 2, cy + dh / 2), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
    }
    bool filmClick(int x, int y) {
        if (!filmH()) return false;
        RECT fr = filmRect();
        if (y < fr.top || y >= fr.bottom) return false;
        int first, visible, cw;
        filmLayout(first, visible, cw);
        int i = (x - fr.left - S(4)) / cw;
        if (i >= 0 && i < visible && first + i < (int)files.size()) go(first + i);
        return true;
    }

    // ---------------- 文件操作
    void pushUndo(int kind, const std::wstring& a, const std::wstring& b) {
        undoStack.push_back({kind, a, b});
        size_t mx = (size_t)std::max(1, cfg.getInt("Archive/UndoCount", 30));
        while (undoStack.size() > mx) undoStack.erase(undoStack.begin());
    }
    int findFile(const std::wstring& p) const {
        std::wstring l = lowerW(p);
        for (size_t i = 0; i < files.size(); ++i) if (lowerW(files[i]) == l) return (int)i;
        return -1;
    }
    void dropCurrentFromList() {
        if (idx < 0 || idx >= (int)files.size()) return;
        cache.erase(files[idx]);
        files.erase(files.begin() + idx);
        cur.reset();
        bitmap.Reset();
        animBmps.clear();
        if (files.empty()) { idx = -1; updateTitle(); invalidate(); return; }
        if (idx >= (int)files.size()) idx = (int)files.size() - 1;
        showNow();
    }
    void insertAtCurrent(const std::wstring& p) {   // 撤销后把文件放回列表并显示
        if (idx < 0) idx = 0;
        int f = findFile(p);
        if (f < 0) { idx = std::min(idx, (int)files.size()); files.insert(files.begin() + idx, p); }
        else idx = f;
        cache.erase(p);
        cur.reset();
        bitmap.Reset();
        animBmps.clear();
        showNow();
    }
    bool badName(const std::wstring& n) const {
        return n.empty() || n.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos;
    }
    void renameCurrent() {
        if (idx < 0 || idx >= (int)files.size()) return;
        std::wstring old = files[idx], name = baseName(old), nn = name;
        size_t dot = name.find_last_of(L'.');
        int stem = dot == std::wstring::npos || dot == 0 ? (int)name.size() : (int)dot;
        if (!promptText(hwnd, L"重命名", L"新文件名:", nn, stem)) return;
        while (!nn.empty() && nn.back() == L' ') nn.pop_back();
        if (nn == name) return;
        if (badName(nn)) { MessageBoxW(hwnd, L"文件名不能为空,也不能包含 \\ / : * ? \" < > |", L"Xview", MB_OK | MB_ICONWARNING); return; }
        std::wstring np = parentDir(old) + L"\\" + nn;
        if (GetFileAttributesW(np.c_str()) != INVALID_FILE_ATTRIBUTES && lowerW(np) != lowerW(old)) {
            MessageBoxW(hwnd, L"已经有同名文件了。", L"Xview", MB_OK | MB_ICONWARNING);
            return;
        }
        if (!MoveFileW(old.c_str(), np.c_str())) { MessageBoxW(hwnd, L"重命名失败(文件被占用或没有权限)。", L"Xview", MB_OK | MB_ICONERROR); return; }
        pushUndo(1, old, np);
        cache.erase(old);
        files[idx] = np;
        cur.reset();
        bitmap.Reset();
        animBmps.clear();
        showNow();
    }
    std::wstring pickFolder(const wchar_t* title) {
        BROWSEINFOW bi;
        ZeroMemory(&bi, sizeof bi);
        bi.hwndOwner = hwnd;
        bi.lpszTitle = title;
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
        if (!pidl) return L"";
        wchar_t buf[MAX_PATH * 2] = L"";
        bool ok = SHGetPathFromIDListW(pidl, buf) != FALSE;
        CoTaskMemFree(pidl);
        return ok ? std::wstring(buf) : std::wstring();
    }
    void copyMoveCurrent(bool move) {
        if (idx < 0 || idx >= (int)files.size()) return;
        std::wstring p = files[idx];
        std::wstring dest = pickFolder(move ? L"移动到文件夹" : L"复制到文件夹");
        if (dest.empty()) return;
        while (dest.size() > 3 && (dest.back() == L'\\' || dest.back() == L'/')) dest.pop_back();
        std::wstring np = dest + (dest.back() == L'\\' ? L"" : L"\\") + baseName(p);
        std::wstring toArg = dest;
        FILEOP_FLAGS flags = FOF_ALLOWUNDO;
        bool exists = GetFileAttributesW(np.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (exists) {
            if (lowerW(np) == lowerW(p)) { if (move) return; }   // 复制到自己所在的文件夹:交给系统(它会生成\"- 副本\")
            else {
                if (cfg.getBool("Archive/SkipIfIdentical", true) && sameFileContent(p, np)) return;
                std::string act = cfg.get("Archive/DuplicateAction", "Ask");
                if (act == "Skip") return;
                if (act == "Overwrite") flags |= FOF_NOCONFIRMATION;
                else if (act == "AutoRename") { np = uniqueName(dest, baseName(p), W(cfg.get("Archive/RenameSeparator", "_"))); toArg = np; }
            }
        }
        std::wstring from = p;
        from.push_back(L'\0');
        std::wstring to = toArg;
        to.push_back(L'\0');
        SHFILEOPSTRUCTW op;
        ZeroMemory(&op, sizeof op);
        op.hwnd = hwnd;
        op.wFunc = move ? FO_MOVE : FO_COPY;
        op.pFrom = from.c_str();
        op.pTo = to.c_str();
        op.fFlags = flags;
        if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted) return;
        if (!move) return;
        if (GetFileAttributesW(np.c_str()) != INVALID_FILE_ATTRIBUTES) pushUndo(1, p, np);
        if (cfg.getBool("Viewer/ExitAfterDelete", false)) { closeWindow(); return; }
        dropCurrentFromList();
    }
    bool restoreFromRecycleBin(const std::wstring& path) {
        PIDLIST_ABSOLUTE bin = nullptr;
        if (FAILED(SHGetSpecialFolderLocation(hwnd, CSIDL_BITBUCKET, &bin)) || !bin) return false;
        ComPtr<IShellFolder> desk;
        ComPtr<IShellFolder2> fold;
        bool done = false;
        if (SUCCEEDED(SHGetDesktopFolder(&desk)) && SUCCEEDED(desk->BindToObject(bin, nullptr, IID_PPV_ARGS(&fold)))) {
            ComPtr<IEnumIDList> en;
            if (SUCCEEDED(fold->EnumObjects(hwnd, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, &en)) && en) {
                std::wstring bn = lowerW(baseName(path)), dir = lowerW(parentDir(path));
                size_t dot = bn.find_last_of(L'.');
                std::wstring stem = dot == std::wstring::npos ? bn : bn.substr(0, dot);
                PITEMID_CHILD item = nullptr, pick = nullptr;
                ULONG got = 0;
                while (en->Next(1, &item, &got) == S_OK && got == 1) {
                    wchar_t nm[MAX_PATH * 2] = L"", loc[MAX_PATH * 2] = L"";
                    STRRET sr;
                    if (SUCCEEDED(fold->GetDisplayNameOf(item, SHGDN_NORMAL, &sr))) StrRetToBufW(&sr, item, nm, MAX_PATH * 2);
                    SHELLDETAILS sd;
                    ZeroMemory(&sd, sizeof sd);
                    if (SUCCEEDED(fold->GetDetailsOf(item, 1, &sd))) StrRetToBufW(&sd.str, item, loc, MAX_PATH * 2);
                    std::wstring n = lowerW(nm), l = lowerW(loc);
                    while (!l.empty() && (l.back() == L'\\' || l.back() == L'/')) l.pop_back();
                    bool same = (n == bn || n == stem) && l == dir;
                    if (same) { if (pick) CoTaskMemFree(pick); pick = item; } else CoTaskMemFree(item);
                    item = nullptr;
                }
                if (pick) {
                    ComPtr<IContextMenu> cm;
                    PCUITEMID_CHILD arr[1] = {pick};
                    if (SUCCEEDED(fold->GetUIObjectOf(hwnd, 1, arr, IID_IContextMenu, nullptr, (void**)&cm)) && cm) {
                        CMINVOKECOMMANDINFO ci;
                        ZeroMemory(&ci, sizeof ci);
                        ci.cbSize = sizeof ci;
                        ci.hwnd = hwnd;
                        ci.lpVerb = "undelete";
                        ci.nShow = SW_SHOWNORMAL;
                        done = SUCCEEDED(cm->InvokeCommand(&ci));
                    }
                    CoTaskMemFree(pick);
                }
            }
        }
        CoTaskMemFree(bin);
        return done;
    }
    void undoLast() {
        if (undoStack.empty()) { MessageBoxW(hwnd, L"没有可以撤销的操作。", L"Xview", MB_OK | MB_ICONINFORMATION); return; }
        UndoOp op = undoStack.back();
        undoStack.pop_back();
        if (op.kind == 1) {
            if (GetFileAttributesW(op.b.c_str()) == INVALID_FILE_ATTRIBUTES) { MessageBoxW(hwnd, L"找不到要还原的文件了。", L"Xview", MB_OK | MB_ICONWARNING); return; }
            if (GetFileAttributesW(op.a.c_str()) != INVALID_FILE_ATTRIBUTES && lowerW(op.a) != lowerW(op.b)) { MessageBoxW(hwnd, L"原来的位置已经有同名文件,没法还原。", L"Xview", MB_OK | MB_ICONWARNING); return; }
            if (!MoveFileW(op.b.c_str(), op.a.c_str())) { MessageBoxW(hwnd, L"撤销失败。", L"Xview", MB_OK | MB_ICONERROR); return; }
            int f = findFile(op.b);
            if (f >= 0) { files[f] = op.a; idx = f; cache.erase(op.b); cache.erase(op.a); cur.reset(); bitmap.Reset(); animBmps.clear(); showNow(); }
            else insertAtCurrent(op.a);
        } else if (op.kind == 2) {
            if (!restoreFromRecycleBin(op.a)) { MessageBoxW(hwnd, L"没能从回收站还原(可能已被清空),请到回收站手工还原。", L"Xview", MB_OK | MB_ICONWARNING); return; }
            insertAtCurrent(op.a);
        }
    }
    void saveAsDialog() {
        if (!cur || !cur->ok || !cur->bmp) return;
        wchar_t buf[MAX_PATH * 2] = L"";
        std::wstring base = (idx >= 0 && idx < (int)files.size()) ? baseName(files[idx]) : L"image";
        size_t dot = base.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) base = base.substr(0, dot);
        wcsncpy(buf, base.c_str(), MAX_PATH * 2 - 1);
        std::wstring filter;
        auto add = [&](const wchar_t* a, const wchar_t* b) { filter += a; filter.push_back(L'\0'); filter += b; filter.push_back(L'\0'); };
        add(L"PNG 图片 (*.png)", L"*.png");
        add(L"JPEG 图片 (*.jpg)", L"*.jpg");
        add(L"BMP 图片 (*.bmp)", L"*.bmp");
        add(L"TIFF 图片 (*.tif)", L"*.tif");
        std::wstring initDir = (idx >= 0 && idx < (int)files.size()) ? parentDir(files[idx]) : L"";
        OPENFILENAMEW ofn;
        ZeroMemory(&ofn, sizeof ofn);
        ofn.lStructSize = sizeof ofn;
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = filter.c_str();
        ofn.nFilterIndex = 1;
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH * 2;
        ofn.lpstrInitialDir = initDir.empty() ? nullptr : initDir.c_str();
        ofn.lpstrDefExt = L"png";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
        if (!GetSaveFileNameW(&ofn)) return;
        std::wstring out = buf;
        std::wstring e = extOf(out);
        if (e != L"png" && e != L"jpg" && e != L"jpeg" && e != L"bmp" && e != L"tif" && e != L"tiff") {
            static const wchar_t* ex[] = {L".png", L".jpg", L".bmp", L".tif"};
            out += ex[std::max(1, std::min(4, (int)ofn.nFilterIndex)) - 1];
        }
        UINT rw = cur->texW, rh = cur->texH;
        auto rsrc = rotatedForSave(cur->bmp.Get(), rw, rh, rotDeg, rw, rh);   // 另存为带上当前的旋转角度
        if (!rsrc || !saveBitmapFile(rsrc.Get(), rw, rh, out)) MessageBoxW(hwnd, L"保存失败。", L"Xview", MB_OK | MB_ICONERROR);
    }
    void setWallpaper() {
        if (idx < 0 || idx >= (int)files.size()) return;
        std::wstring p = files[idx], e = extOf(p);
        if (!(e == L"bmp" || e == L"jpg" || e == L"jpeg" || e == L"png") || std::fabs(std::fmod(rotDeg, 360.0)) > 1e-6) {   // 旋转过的图要先存成临时图再设
            if (!cur || !cur->ok) return;
            wchar_t tmp[MAX_PATH];
            GetTempPathW(MAX_PATH, tmp);
            p = std::wstring(tmp) + L"xview_wallpaper.png";
            UINT rw = cur->texW, rh = cur->texH;
            auto rsrc = rotatedForSave(cur->bmp.Get(), rw, rh, rotDeg, rw, rh);
            if (!rsrc || !saveBitmapFile(rsrc.Get(), rw, rh, p)) { MessageBoxW(hwnd, L"设置壁纸失败。", L"Xview", MB_OK | MB_ICONERROR); return; }
        }
        SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, const_cast<wchar_t*>(p.c_str()), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    }
    void printCurrent() {
        if (idx < 0 || idx >= (int)files.size()) return;
        HINSTANCE r;
        wchar_t pr[256] = L"";
        DWORD pn = 256;
        if (!cfg.getBool("Misc/PrintShowSetup", true) && GetDefaultPrinterW(pr, &pn)) {   // 不弹打印设置:直接送默认打印机
            std::wstring args = L"\"" + std::wstring(pr) + L"\"";
            r = ShellExecuteW(hwnd, L"printto", files[idx].c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
        } else {
            r = ShellExecuteW(hwnd, L"print", files[idx].c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        if ((INT_PTR)r <= 32) MessageBoxW(hwnd, L"系统里没有可以打印这种图片的程序。", L"Xview", MB_OK | MB_ICONINFORMATION);
    }
    void rotatePrompt() {
        if (!hasImage()) return;
        std::wstring t = L"90";
        if (!promptText(hwnd, L"旋转", L"顺时针旋转的角度(负数为逆时针):", t)) return;
        double d = wcstod(t.c_str(), nullptr);
        if (d != 0.0) rotateBy(d);
    }
    void aboutBox() {
        MessageBoxW(hwnd, L"Xview 原生看图\n\n纯 C++ / Direct2D,启动快、不依赖运行库。\n配置与 Xdir 共用 data\\config\\settings.ini。", L"关于 Xview", MB_OK | MB_ICONINFORMATION);
    }

    // ---------------- 交给 Xdir 办的命令(评分/色标/收藏存在 Xdir 的 SQLite 里,选项窗口是 Qt 写的)
    static bool isXdirCmd(const std::string& c) {
        static const char* const k[] = {"Options", "ManageExtensions", "FileAssoc", "FileUnassoc", "CommandPalette",
                                        "Rate0", "Rate1", "Rate2", "Rate3", "Rate4", "Rate5",
                                        "LabelRed", "LabelYellow", "LabelGreen", "LabelBlue", "ToggleFavorite", "ShowFavorites"};
        for (auto* x : k) if (c == x) return true;
        return false;
    }
    static bool xdirRunning() {
        if (WaitNamedPipeW(L"\\\\.\\pipe\\Xdir-80101", 1)) return true;
        return GetLastError() != ERROR_FILE_NOT_FOUND;
    }
    void sendXdir(const std::string& c) {
        if (!xdirRunning()) {
            MessageBoxW(hwnd, L"这个功能由 Xdir 来完成,但 Xdir 现在没有在运行。\n请先打开 Xdir,再从 Xdir 里打开图片。", L"Xview", MB_OK | MB_ICONINFORMATION);
            return;
        }
        std::wstring dir = root + L"\\data\\config\\native_cmd";
        CreateDirectoryW((root + L"\\data").c_str(), nullptr);
        CreateDirectoryW((root + L"\\data\\config").c_str(), nullptr);
        CreateDirectoryW(dir.c_str(), nullptr);
        static unsigned seq = 0;
        std::wstring base = dir + L"\\" + std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(++seq);
        std::string path = (idx >= 0 && idx < (int)files.size()) ? U(files[idx]) : std::string();
        writeFileText(base + L".tmp", c + "\n" + path + "\n");
        MoveFileExW((base + L".tmp").c_str(), (base + L".txt").c_str(), MOVEFILE_REPLACE_EXISTING);
        if (c == "Options" || c == "ManageExtensions" || c == "FileAssoc" || c == "FileUnassoc" || c == "CommandPalette") {
            // 弹窗在 Xdir 那边出现:把它带到前台
            HWND x = FindWindowW(nullptr, L"Xdir");
            if (x) SetForegroundWindow(x);
        }
    }

    // ---------------- 深色标题栏 / 自动调整窗口 / 添加到列表
    void applyTitleBar() {
        if (!hwnd) return;
        std::string m = cfg.get("Display/DarkTitleBar", "Auto");
        bool dark;
        if (m == "On") dark = true;
        else if (m == "Off") dark = false;
        else {
            double lum = (0.2126 * ((th.bg >> 16) & 0xFF) + 0.7152 * ((th.bg >> 8) & 0xFF) + 0.0722 * (th.bg & 0xFF)) / 255.0;
            dark = lum < 0.5;
        }
        if ((int)dark == darkApplied) return;
        darkApplied = dark ? 1 : 0;
        BOOL v = dark ? TRUE : FALSE;
        if (FAILED(DwmSetWindowAttribute(hwnd, 20, &v, sizeof v))) DwmSetWindowAttribute(hwnd, 19, &v, sizeof v);
    }
    void resizeToImage() {
        if (!cfg.getBool("Viewer/AutoResizeWindow", false) || fullscreen || !hwnd || IsZoomed(hwnd) || IsIconic(hwnd) || !cur || !cur->ok) return;
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
        RECT wa = mi.rcWork;
        DWORD st = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), ex = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        RECT nc = {0, 0, 0, 0};
        AdjustWindowRectExForDpi(&nc, st, GetMenu(hwnd) != nullptr, ex, (UINT)dpi);
        int extraW = nc.right - nc.left, extraH = (nc.bottom - nc.top) + tbH() + stH() + filmH();
        double availW = std::max(100, (int)(wa.right - wa.left) - extraW), availH = std::max(100, (int)(wa.bottom - wa.top) - extraH);
        double w = effW(), h = effH(), sc = 1.0;
        double fitSc = std::min(availW / w, availH / h);
        if (fitSc < 1.0) { if (cfg.getBool("Viewer/ShrinkToScreen", true)) sc = fitSc; else sc = 1.0; }
        else if (cfg.getBool("Viewer/EnlargeToScreen", true)) sc = fitSc;
        int cw = std::max(240, std::min((int)availW, (int)(w * sc + 0.5))), ch = std::max(120, std::min((int)availH, (int)(h * sc + 0.5)));
        int ww = cw + extraW, wh = ch + extraH;
        RECT cr;
        GetWindowRect(hwnd, &cr);
        int x = cr.left, y = cr.top;
        if (cfg.getBool("Viewer/CenterOnResize", true)) { x = wa.left + ((wa.right - wa.left) - ww) / 2; y = wa.top + ((wa.bottom - wa.top) - wh) / 2; }
        x = std::max<int>(wa.left, std::min<int>(x, wa.right - ww));
        y = std::max<int>(wa.top, std::min<int>(y, wa.bottom - wh));
        SetWindowPos(hwnd, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    void addPath(std::wstring p, bool jump) {   // Misc/OnStartFile=AddToList / Misc/OnDropFile=Add:加进现有列表,不替换
        p = absPath(p);
        std::vector<std::wstring> add;
        if (dirExists(p)) {
            std::vector<std::wstring> old = files;
            listDir(p);
            add = files;
            files = old;
        } else if (fileExists(p)) add.push_back(p);
        else return;
        if (files.empty()) { openPath(p); return; }
        int first = -1;
        for (auto& f : add) {
            int k = findFile(f);
            if (k < 0) { files.push_back(f); k = (int)files.size() - 1; }
            if (first < 0) first = k;
        }
        if (jump && first >= 0) go(first);
        else { updateTitle(); invalidate(); }
    }

    // ---------------- 上一个 / 下一个文件夹(同一级的兄弟文件夹里第一个有图片的)
    void siblingFolder(int dir) {
        if (idx < 0 || idx >= (int)files.size()) return;
        std::wstring curDir = parentDir(files[idx]);
        std::wstring par = parentDir(curDir);
        if (par.empty()) return;
        std::vector<std::wstring> dirs;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((par + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                dirs.push_back(par + L"\\" + fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(dirs.begin(), dirs.end(), [](const std::wstring& a, const std::wstring& b) { return StrCmpLogicalW(baseName(a).c_str(), baseName(b).c_str()) < 0; });
        int at = -1;
        for (size_t i = 0; i < dirs.size(); ++i) if (lowerW(dirs[i]) == lowerW(curDir)) { at = (int)i; break; }
        if (at < 0) return;
        std::vector<std::wstring> oldFiles = files;
        int oldIdx = idx;
        for (int k = at + dir; k >= 0 && k < (int)dirs.size(); k += dir) {
            listDir(dirs[k]);
            if (!files.empty()) {
                idx = dir > 0 ? 0 : (int)files.size() - 1;
                cur.reset();
                bitmap.Reset();
                animBmps.clear();
                showNow();
                return;
            }
        }
        files = oldFiles;
        idx = oldIdx;
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
        else if (c == "PrevFolder") siblingFolder(-1);
        else if (c == "NextFolder") siblingFolder(1);
        else if (c == "RotateBy") rotatePrompt();
        else if (c == "SaveAs") saveAsDialog();
        else if (c == "CopyTo") copyMoveCurrent(false);
        else if (c == "MoveTo") copyMoveCurrent(true);
        else if (c == "Rename") renameCurrent();
        else if (c == "Undo") undoLast();
        else if (c == "SetWallpaper") setWallpaper();
        else if (c == "Print") printCurrent();
        else if (c == "About") aboutBox();
        else if (c == "ToggleMenu") { showMenu = !showMenu; applyMenu(); if (fitAuto) refit(); else clampOffset(); invalidate(); }
        else if (c == "ToggleKeyHints") { keyHints = !keyHints; invalidate(); }
        else if (c == "ToggleFilmstrip") { showFilm = !showFilm; if (fitAuto) refit(); else clampOffset(); invalidate(); }
        else if (c == "ToggleSlideshow") toggleSlideshow();
        else if (isXdirCmd(c)) sendXdir(c);
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
    // 把"正在看哪张 / 是否已退出"写进 data\config\native_last.txt,目录页(Xdir)读它来同步选中项。
    // 第一行 open 或 closed,第二行完整路径。先写临时文件再替换,读的一方不会读到半截。
    void reportState(bool closed) {
        if (idx < 0 || idx >= (int)files.size()) return;
        CreateDirectoryW((root + L"\\data").c_str(), nullptr);
        CreateDirectoryW((root + L"\\data\\config").c_str(), nullptr);
        std::wstring f = root + L"\\data\\config\\native_last.txt";
        std::wstring t = f + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
        writeFileText(t, std::string(closed ? "closed" : "open") + "\n" + U(files[idx]) + "\n");
        MoveFileExW(t.c_str(), f.c_str(), MOVEFILE_REPLACE_EXISTING);
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
        std::string out = "; Xview 原生版自己的记忆(窗口位置)。可以删除,不影响其他设置。\n[Native]\n";
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
        case WM_APP_THUMB:
            a->invalidate();
            return 0;
        case WM_TIMER:
            if (wp == TIMER_ANIM) a->stepAnim();
            else if (wp == TIMER_SHOW) a->slideStep();
            else if (wp == TIMER_FLIP) { KillTimer(hwnd, TIMER_FLIP); a->showNow(); }
            else if (wp == TIMER_CURSOR) {
                if (a->fullscreen && a->cfg.getBool("Viewer/HideMouseInFullscreen", true) && GetTickCount() - a->lastMouse > (DWORD)std::max(200, a->cfg.getInt("Viewer/HideMouseDelayMs", 2000))) a->showCursor(false);
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
            if (a->filmClick(x, y)) return 0;
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
            if (!a->dblPending.empty()) {
                std::string cmd = a->dblPending;
                a->dblPending.clear();
                ReleaseCapture();
                a->run(cmd);
                return 0;
            }
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
        case WM_LBUTTONDBLCLK: {   // 双击的命令(例如退出)等鼠标松开再执行:不然松开的那一下会落到下面的窗口上,把对方弄成"按住拖拽"的状态
            if (GET_Y_LPARAM(lp) >= a->tbH()) { a->dblPending = a->cfg.get("Mouse/DoubleClick"); SetCapture(hwnd); }
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
            UINT nDrop = DragQueryFileW((HDROP)wp, 0xFFFFFFFF, nullptr, 0);
            bool addMode = a->cfg.get("Misc/OnDropFile", "Add") == "Add";
            for (UINT di = 0; di < nDrop; ++di) {
                if (!DragQueryFileW((HDROP)wp, di, buf, MAX_PATH * 4)) continue;
                if (di == 0 && !addMode) { a->openPath(buf); break; }
                a->addPath(buf, di == 0);
            }
            DragFinish((HDROP)wp);
            return 0;
        }
        case WM_COPYDATA: {
            auto* cd = reinterpret_cast<COPYDATASTRUCT*>(lp);
            if (cd && (cd->dwData == kCopyDataPath || cd->dwData == kCopyDataPathAdd) && cd->lpData && cd->cbData >= sizeof(wchar_t)) {
                std::wstring p((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t));
                while (!p.empty() && p.back() == L'\0') p.pop_back();
                if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
                SetForegroundWindow(hwnd);
                if (!p.empty()) {
                    if (cd->dwData == kCopyDataPathAdd || a->cfg.get("Misc/OnStartFile", "AddToList") == "AddToList") a->addPath(p, true);
                    else a->openPath(p);
                }
                return TRUE;
            }
            return FALSE;
        }
        case WM_CLOSE:
            a->reportState(true);
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
    // 其他 "--xxx"(原来 Python 版的选项对话框用的 --list-formats / --assoc-machine 等):原生版没有这些功能,明说,不再转交别的程序
    if (!flags.empty() && paths.empty()) {
        std::wstring msg = L"原生版 Xview 不支持这些命令行选项:";
        for (auto& f : flags) msg += L"\n  " + f;
        MessageBoxW(nullptr, msg.c_str(), L"Xview", MB_ICONINFORMATION);
        return 2;
    }
    // Misc/OnStartFile=NewWindow:不交给已有窗口,自己开一个新的
    if (!newWindow && !paths.empty()) {
        std::string ini = readFileText(root + L"\\data\\config\\settings.ini");
        size_t at = ini.find("\nOnStartFile=");
        if (at != std::string::npos && ini.compare(at + 13, 9, "NewWindow") == 0) newWindow = true;
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
    a.applyTitleBar();
    DragAcceptFiles(hwnd, TRUE);
    a.pool.start(std::max(1, std::min(16, a.cfg.getInt("Viewer/LoadThreads", 2))), poolWork);
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
