// INI 读取:读 app/config/settings.ini(出厂模板)和 data/config/settings.ini(你的设置),后者覆盖前者。
// 键名统一成 "段/键",例如 "Viewer/PanStep"。原生版只读这两个文件,绝不写它们(Python 版会校验未知项)。
#pragma once
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace xv {

inline std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (unsigned char)s[b] <= ' ') ++b;
    while (e > b && (unsigned char)s[e - 1] <= ' ') --e;
    return s.substr(b, e - b);
}

inline std::vector<std::string> splitList(const std::string& v, char sep = ',') {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= v.size()) {
        size_t j = v.find(sep, i);
        if (j == std::string::npos) j = v.size();
        std::string t = trim(v.substr(i, j - i));
        if (!t.empty()) out.push_back(t);
        i = j + 1;
    }
    return out;
}

inline std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

class Ini {
public:
    std::map<std::string, std::string> kv;

    void loadText(std::string text) {
        if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
        std::string section;
        size_t i = 0;
        while (i < text.size()) {
            size_t j = text.find('\n', i);
            if (j == std::string::npos) j = text.size();
            std::string line = trim(text.substr(i, j - i));
            i = j + 1;
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            if (line[0] == '[') {
                size_t e = line.find(']');
                if (e != std::string::npos) section = trim(line.substr(1, e - 1));
                continue;
            }
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = trim(line.substr(0, eq));
            if (key.empty()) continue;
            kv[section + "/" + key] = trim(line.substr(eq + 1));
        }
    }

    bool has(const std::string& k) const { return kv.count(k) != 0; }
    std::string get(const std::string& k, const std::string& def = "") const {
        auto it = kv.find(k);
        return it == kv.end() ? def : it->second;
    }
    bool getBool(const std::string& k, bool def) const {
        auto it = kv.find(k);
        if (it == kv.end()) return def;
        std::string v = lower(it->second);
        return v == "1" || v == "true" || v == "yes" || v == "on";
    }
    int getInt(const std::string& k, int def) const {
        auto it = kv.find(k);
        if (it == kv.end() || it->second.empty()) return def;
        return (int)std::strtol(it->second.c_str(), nullptr, 10);
    }
    double getDouble(const std::string& k, double def) const {
        auto it = kv.find(k);
        if (it == kv.end() || it->second.empty()) return def;
        return std::strtod(it->second.c_str(), nullptr);
    }
    std::vector<std::string> getList(const std::string& k) const { return splitList(get(k)); }
};

}  // namespace xv
