// 快捷键:把配置里的写法("Ctrl+O"、"F2"、"Space"、"+")变成可查表的键。数值和 Windows 的 VK_ 码一致,
// 所以这个文件不依赖 windows.h,可以在任何系统上单独测试。
#pragma once
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ini.h"

namespace xv {

enum { KM_CTRL = 1, KM_ALT = 2, KM_SHIFT = 4 };

struct KeySpec {
    int vk = 0;        // vk>0:按虚拟键匹配(含修饰键)
    char ch = 0;       // ch!=0:按输入的字符匹配(+ - * / = 这类标点,不管 Shift)
    int mods = 0;
    int vk2 = 0;       // 小键盘的同义键(例如 + 的 VK_ADD)
};

inline bool parseKeySpec(const std::string& specIn, KeySpec& out) {
    std::string s = trim(specIn);
    out = KeySpec();
    for (;;) {  // 吃掉前面的修饰键;"Ctrl++" 里最后那个 + 是键本身
        std::string l = lower(s);
        if (l.compare(0, 5, "ctrl+") == 0 && s.size() > 5) { out.mods |= KM_CTRL; s = s.substr(5); }
        else if (l.compare(0, 4, "alt+") == 0 && s.size() > 4) { out.mods |= KM_ALT; s = s.substr(4); }
        else if (l.compare(0, 6, "shift+") == 0 && s.size() > 6) { out.mods |= KM_SHIFT; s = s.substr(6); }
        else if (l.compare(0, 5, "meta+") == 0 && s.size() > 5) { s = s.substr(5); }
        else break;
    }
    if (s.empty()) return false;
    std::string l = lower(s);
    static const std::map<std::string, int> names = {
        {"esc", 27}, {"escape", 27}, {"return", 13}, {"enter", 13}, {"space", 32}, {"backspace", 8},
        {"tab", 9}, {"backtab", 9}, {"delete", 46}, {"del", 46}, {"insert", 45}, {"ins", 45},
        {"home", 36}, {"end", 35}, {"pageup", 33}, {"pgup", 33}, {"pagedown", 34}, {"pgdown", 34}, {"pgdn", 34},
        {"left", 37}, {"up", 38}, {"right", 39}, {"down", 40}};
    auto it = names.find(l);
    if (it != names.end()) { out.vk = it->second; return true; }
    if (l.size() >= 2 && l[0] == 'f') {
        int n = std::atoi(l.c_str() + 1);
        if (n >= 1 && n <= 24) { out.vk = 0x70 + n - 1; return true; }
    }
    if (s.size() == 1) {
        char c = s[0];
        if (c >= 'a' && c <= 'z') { out.vk = c - 'a' + 'A'; return true; }
        if (c >= 'A' && c <= 'Z') { out.vk = c; return true; }
        if (c >= '0' && c <= '9') { out.vk = c; return true; }
        // 标点
        int vk = 0, vk2 = 0;
        switch (c) {
            case '+': vk = 0xBB; vk2 = 0x6B; break;
            case '=': vk = 0xBB; break;
            case '-': vk = 0xBD; vk2 = 0x6D; break;
            case '*': vk = 0x6A; break;
            case '/': vk = 0xBF; vk2 = 0x6F; break;
            case ',': vk = 0xBC; break;
            case '.': vk = 0xBE; vk2 = 0x6E; break;
            case ';': vk = 0xBA; break;
            case '[': vk = 0xDB; break;
            case ']': vk = 0xDD; break;
            case '\\': vk = 0xDC; break;
            case '\'': vk = 0xDE; break;
            case '`': vk = 0xC0; break;
            default: return false;
        }
        if (out.mods & (KM_CTRL | KM_ALT)) { out.vk = vk; out.vk2 = vk2; }  // 带 Ctrl/Alt 时 Windows 不发字符,走虚拟键
        else out.ch = c;
        return true;
    }
    return false;
}

class Keymap {
public:
    // 同一个键被多个命令占用时,先加的赢(调用者按"查看窗口用得到的命令"顺序加)
    void add(const std::string& cmd, const std::vector<std::string>& specs) {
        for (auto& sp : specs) {
            KeySpec k;
            if (!parseKeySpec(sp, k)) continue;
            if (k.ch) byChar_.insert({k.ch, cmd});
            if (k.vk) byVk_.insert({{k.vk, k.mods}, cmd});
            if (k.vk2) byVk_.insert({{k.vk2, k.mods}, cmd});
        }
    }
    const std::string* byVk(int vk, int mods) const {
        auto it = byVk_.find({vk, mods});
        return it == byVk_.end() ? nullptr : &it->second;
    }
    const std::string* byChar(char c) const {
        auto it = byChar_.find(c);
        return it == byChar_.end() ? nullptr : &it->second;
    }
    // 命令的第一个快捷键(右键菜单里显示用),没有返回空串
    std::string firstKeyText(const std::string& cmd, const Ini& ini) const {
        auto v = ini.getList("Keys/" + cmd);
        return v.empty() ? std::string() : v[0];
    }
    size_t size() const { return byVk_.size() + byChar_.size(); }

private:
    std::map<std::pair<int, int>, std::string> byVk_;
    std::map<char, std::string> byChar_;
};

}  // namespace xv
