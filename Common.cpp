#include "Common.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace bd {

// ---------------------------------------------------------------- logging
// Logging is intentionally lightweight. Messages go to the terminal, and warnings/errors
// are also kept in memory so the game can show them in its own report screen.
std::vector<std::string> &Log::warnings() {
    static std::vector<std::string> w;
    return w;
}
static void vlog(const char *level, const char *fmt, va_list ap, bool keep) {
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    fprintf(stderr, "[%s] %s\n", level, buf);
    if (keep && Log::warnings().size() < 200)
        Log::warnings().push_back(buf);
}
void Log::info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog("info", fmt, ap, false);
    va_end(ap);
}
void Log::warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog("warn", fmt, ap, true);
    va_end(ap);
}
void Log::error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog("ERROR", fmt, ap, true);
    va_end(ap);
}

// ---------------------------------------------------------------- files
// These helpers cover the small file operations used throughout the project. Keeping
// them here gives config, save and asset code one consistent way to read and write data.
namespace util {
bool readFile(const std::string &path, std::string &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}
// Write to a temporary file first, then rename it into place. That way a crash or
// interrupted write is less likely to leave a half-written save file behind.
bool writeFile(const std::string &path, const std::string &data) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        f.write(data.data(), (std::streamsize)data.size());
        if (!f)
            return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) { // e.g. Windows refusing to overwrite
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
        if (ec)
            return false;
    }
    return true;
}
bool fileExists(const std::string &path) {
    std::error_code ec;
    return fs::exists(path, ec) && fs::is_regular_file(path, ec);
}
std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a]))
        a++;
    while (b > a && isspace((unsigned char)s[b - 1]))
        b--;
    return s.substr(a, b - a);
}
std::string replaceAll(std::string s, const std::string &from, const std::string &to) {
    if (from.empty())
        return s;
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) {
        s.replace(p, from.size(), to);
        p += to.size();
    }
    return s;
}
std::vector<std::string> splitLines(const std::string &s) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) {
        if (c == '\r')
            continue;
        if (c == '\n') {
            v.push_back(cur);
            cur.clear();
        } else
            cur += c;
    }
    v.push_back(cur);
    return v;
}
// Move through UTF-8 one character at a time. A character can take more than one byte,
// so these helpers keep dialogue typing and text wrapping from splitting a character.
uint32_t nextCodepoint(const std::string &s, size_t &i) {
    unsigned char c = (unsigned char)s[i];
    uint32_t cp;
    int extra;
    if (c < 0x80) {
        cp = c;
        extra = 0;
    } else if ((c >> 5) == 6) {
        cp = c & 0x1F;
        extra = 1;
    } else if ((c >> 4) == 14) {
        cp = c & 0x0F;
        extra = 2;
    } else if ((c >> 3) == 30) {
        cp = c & 0x07;
        extra = 3;
    } else {
        i++;
        return '?';
    }
    i++;
    for (int k = 0; k < extra; k++) {
        if (i >= s.size() || ((unsigned char)s[i] & 0xC0) != 0x80)
            return '?';
        cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
        i++;
    }
    return cp;
}
size_t utf8Length(const std::string &s) {
    size_t n = 0, i = 0;
    while (i < s.size()) {
        nextCodepoint(s, i);
        n++;
    }
    return n;
}
std::string utf8Substr(const std::string &s, size_t count) {
    size_t i = 0, n = 0;
    while (i < s.size() && n < count) {
        nextCodepoint(s, i);
        n++;
    }
    return s.substr(0, i);
}
std::string toLower(std::string s) {
    for (auto &c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}
} // namespace util

// ---------------------------------------------------------------- paths
// Resolve paths relative to the game folder instead of relying on whichever directory
// the player happened to launch the executable from.
namespace Paths {
static std::string g_root = ".";
static std::string g_saves;

static bool looksLikeRoot(const fs::path &p) {
    std::error_code ec;
    return fs::exists(p / "birthday_config.json", ec) || fs::exists(p / "assets", ec);
}
// Look for the project root near the current working directory and executable. This
// makes the game behave the same whether it is launched from an IDE or a file manager.
void init(const char *argv0) {
    std::vector<fs::path> candidates;
    std::error_code ec;
    candidates.push_back(fs::current_path(ec));
    if (argv0 && *argv0) {
        fs::path exe = fs::absolute(argv0, ec).parent_path();
        for (int up = 0; up < 4; up++) {
            candidates.push_back(exe);
            if (!exe.has_parent_path() || exe.parent_path() == exe)
                break;
            exe = exe.parent_path();
        }
    }
    for (auto &c : candidates)
        if (looksLikeRoot(c)) {
            g_root = c.string();
            break;
        }
    if (g_root == ".")
        g_root = fs::current_path(ec).string();
    Log::info("Game root: %s", g_root.c_str());
}
const std::string &root() {
    return g_root;
}
std::string file(const std::string &rel) {
    if (rel.empty())
        return g_root;
    fs::path p(rel);
    if (p.is_absolute())
        return rel;
    return (fs::path(g_root) / p).string();
}
// Prefer a saves folder beside the game, but fall back to the home directory
// if that location is read-only (for example, when the game is installed in Program Files).
std::string saves() {
    if (!g_saves.empty())
        return g_saves;
    fs::path p = fs::path(g_root) / "saves";
    std::error_code ec;
    fs::create_directories(p, ec);
    // verify writable
    std::string probe = (p / ".probe").string();
    if (!util::writeFile(probe, "x")) {
        const char *home = std::getenv("HOME");
        if (!home)
            home = std::getenv("USERPROFILE");
        p = fs::path(home ? home : ".") / ".birthday_adventure";
        fs::create_directories(p, ec);
    } else {
        fs::remove(probe, ec);
    }
    g_saves = p.string();
    return g_saves;
}
} // namespace Paths

} // namespace bd
