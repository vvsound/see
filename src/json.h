// 最小 JSON 解析(只为读 commands.json / themes/*.json / keymaps/*.json)。
#pragma once
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace xv {

struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    const Json* find(const std::string& k) const {
        for (auto& p : o) if (p.first == k) return &p.second;
        return nullptr;
    }
    std::string str(const std::string& k, const std::string& def = "") const {
        const Json* j = find(k);
        return (j && j->t == Str) ? j->s : def;
    }
    bool boolean(const std::string& k, bool def = false) const {
        const Json* j = find(k);
        return (j && j->t == Bool) ? j->b : def;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : p_(text.c_str()), end_(text.c_str() + text.size()) {
        if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) p_ += 3;
    }
    bool parse(Json& out) {
        ws();
        if (!value(out, 0)) return false;
        ws();
        return p_ == end_;
    }

private:
    const char* p_;
    const char* end_;

    void ws() { while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) ++p_; }

    static void putUtf8(std::string& s, unsigned cp) {
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
        else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    }

    bool hex4(unsigned& v) {
        if (end_ - p_ < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p_++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool string(std::string& out) {
        if (p_ >= end_ || *p_ != '"') return false;
        ++p_;
        while (p_ < end_ && *p_ != '"') {
            char c = *p_++;
            if (c != '\\') { out += c; continue; }
            if (p_ >= end_) return false;
            char e = *p_++;
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                        p_ += 2;
                        unsigned lo;
                        if (!hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    putUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        if (p_ >= end_) return false;
        ++p_;
        return true;
    }

    bool value(Json& j, int depth) {
        if (depth > 64 || p_ >= end_) return false;
        char c = *p_;
        if (c == '{') {
            ++p_;
            j.t = Json::Obj;
            ws();
            if (p_ < end_ && *p_ == '}') { ++p_; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!string(k)) return false;
                ws();
                if (p_ >= end_ || *p_ != ':') return false;
                ++p_;
                ws();
                Json v;
                if (!value(v, depth + 1)) return false;
                j.o.emplace_back(std::move(k), std::move(v));
                ws();
                if (p_ < end_ && *p_ == ',') { ++p_; continue; }
                if (p_ < end_ && *p_ == '}') { ++p_; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++p_;
            j.t = Json::Arr;
            ws();
            if (p_ < end_ && *p_ == ']') { ++p_; return true; }
            for (;;) {
                ws();
                Json v;
                if (!value(v, depth + 1)) return false;
                j.a.push_back(std::move(v));
                ws();
                if (p_ < end_ && *p_ == ',') { ++p_; continue; }
                if (p_ < end_ && *p_ == ']') { ++p_; return true; }
                return false;
            }
        }
        if (c == '"') { j.t = Json::Str; return string(j.s); }
        if (end_ - p_ >= 4 && std::string(p_, 4) == "true") { p_ += 4; j.t = Json::Bool; j.b = true; return true; }
        if (end_ - p_ >= 5 && std::string(p_, 5) == "false") { p_ += 5; j.t = Json::Bool; j.b = false; return true; }
        if (end_ - p_ >= 4 && std::string(p_, 4) == "null") { p_ += 4; j.t = Json::Null; return true; }
        char* e = nullptr;
        double d = std::strtod(p_, &e);
        if (e == p_) return false;
        p_ = e;
        j.t = Json::Num;
        j.n = d;
        return true;
    }
};

inline bool parseJson(const std::string& text, Json& out) { return JsonParser(text).parse(out); }

}  // namespace xv
