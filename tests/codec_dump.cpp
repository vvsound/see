// 测试用:codec_dump psd|hdr|raw 文件 -> 向 stdout 写 "w h bits\n" + BGRA(raw 模式写预览 JPEG 偏移/长度/方向)
#include "codecs.h"
#include <fstream>
#include <iterator>
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::ifstream f(argv[2], std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string t = argv[1];
    xcodec::Image im; std::string err;
    if (t == "raw") {
        size_t o, l;
        if (!xcodec::findRawPreview(d.data(), d.size(), o, l)) { std::printf("FAIL\n"); return 1; }
        std::printf("%zu %zu %d\n", o, l, xcodec::rawOrientation(d.data(), d.size()));
        return 0;
    }
    if (t == "gz" || t == "zlib") {
        std::vector<uint8_t> o;
        bool g = t == "gz" ? xcodec::gunzip(d.data(), d.size(), o, (size_t)64 << 20) : xcodec::inflateZlib(d.data(), d.size(), o, (size_t)64 << 20);
        if (!g) { std::printf("FAIL\n"); return 1; }
        std::fwrite(o.data(), 1, o.size(), stdout);
        return 0;
    }
    bool ok = t == "exr" ? xcodec::decodeExr(d.data(), d.size(), im, err) : t == "psd" ? xcodec::decodePsd(d.data(), d.size(), im, err) : xcodec::decodeHdr(d.data(), d.size(), im, err);
    if (!ok) { std::printf("ERR %s\n", err.c_str()); return 1; }
    std::printf("%d %d %d\n", im.w, im.h, im.bits);
    std::fwrite(im.bgra.data(), 1, im.bgra.size(), stdout);
    return 0;
}
