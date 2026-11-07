#include "Config.h"

#include <cstdlib>
#include <map>
#include <sstream>

namespace bd {

bool parseArea(const std::string &s, AreaId &out) {
    std::string l = util::toLower(util::trim(s));
    for (int i = 0; i <= (int)AreaId::Final; i++)
        if (l == areaKey((AreaId)i)) {
            out = (AreaId)i;
            return true;
        }
    return false;
}

// ================================================================ tiny JSON reader
// Just enough JSON for the config file: objects, arrays, strings, numbers, booleans
// and null. It keeps the project free of an extra dependency.
namespace {
struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    const Json *get(const std::string &k) const {
        for (auto &kv : o)
            if (kv.first == k)
                return &kv.second;
        return nullptr;
    }
    std::string str(const std::string &def = "") const {
        return type == Str ? s : def;
    }
};

struct Parser {
    const std::string &t;
    size_t i = 0;
    std::string err;
    explicit Parser(const std::string &text) : t(text) {}

    void ws() {
        while (i < t.size()) {
            char c = t[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                i++;
            else if (c == '/' && i + 1 < t.size() && t[i + 1] == '/') { // allow // comments
                while (i < t.size() && t[i] != '\n')
                    i++;
            } else
                break;
        }
    }
    bool fail(const char *m) {
        if (err.empty())
            err = std::string(m) + " at offset " + std::to_string(i);
        return false;
    }
    static void putUtf8(std::string &o, uint32_t cp) {
        if (cp < 0x80)
            o += (char)cp;
        else if (cp < 0x800) {
            o += (char)(0xC0 | (cp >> 6));
            o += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            o += (char)(0xE0 | (cp >> 12));
            o += (char)(0x80 | ((cp >> 6) & 0x3F));
            o += (char)(0x80 | (cp & 0x3F));
        } else {
            o += (char)(0xF0 | (cp >> 18));
            o += (char)(0x80 | ((cp >> 12) & 0x3F));
            o += (char)(0x80 | ((cp >> 6) & 0x3F));
            o += (char)(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(uint32_t &v) {
        if (i + 4 > t.size())
            return false;
        v = 0;
        for (int k = 0; k < 4; k++) {
            char c = t[i++];
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= c - '0';
            else if (c >= 'a' && c <= 'f')
                v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                v |= c - 'A' + 10;
            else
                return false;
        }
        return true;
    }
    bool string(std::string &out) {
        if (i >= t.size() || t[i] != '"')
            return fail("expected string");
        i++;
        while (i < t.size() && t[i] != '"') {
            char c = t[i++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= t.size())
                return fail("bad escape");
            char e = t[i++];
            switch (e) {
            case 'n':
                out += '\n';
                break;
            case 't':
                out += '\t';
                break;
            case 'r':
                out += '\r';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case '/':
            case '\\':
            case '"':
                out += e;
                break;
            case 'u': {
                uint32_t cp;
                if (!hex4(cp))
                    return fail("bad \\u escape");
                if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < t.size() && t[i] == '\\' &&
                    t[i + 1] == 'u') { // surrogate pair
                    i += 2;
                    uint32_t lo;
                    if (!hex4(lo))
                        return fail("bad \\u escape");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                putUtf8(out, cp);
                break;
            }
            default:
                return fail("unknown escape");
            }
        }
        if (i >= t.size())
            return fail("unterminated string");
        i++;
        return true;
    }
    bool value(Json &j, int depth = 0) {
        if (depth > 32)
            return fail("nesting too deep");
        ws();
        if (i >= t.size())
            return fail("unexpected end");
        char c = t[i];
        if (c == '{') {
            j.type = Json::Obj;
            i++;
            ws();
            if (i < t.size() && t[i] == '}') {
                i++;
                return true;
            }
            while (true) {
                ws();
                std::string k;
                if (!string(k))
                    return false;
                ws();
                if (i >= t.size() || t[i] != ':')
                    return fail("expected ':'");
                i++;
                Json v;
                if (!value(v, depth + 1))
                    return false;
                j.o.emplace_back(std::move(k), std::move(v));
                ws();
                if (i < t.size() && t[i] == ',') {
                    i++;
                    continue;
                }
                if (i < t.size() && t[i] == '}') {
                    i++;
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            j.type = Json::Arr;
            i++;
            ws();
            if (i < t.size() && t[i] == ']') {
                i++;
                return true;
            }
            while (true) {
                Json v;
                if (!value(v, depth + 1))
                    return false;
                j.a.push_back(std::move(v));
                ws();
                if (i < t.size() && t[i] == ',') {
                    i++;
                    continue;
                }
                if (i < t.size() && t[i] == ']') {
                    i++;
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            j.type = Json::Str;
            return string(j.s);
        }
        if (t.compare(i, 4, "true") == 0) {
            j.type = Json::Bool;
            j.b = true;
            i += 4;
            return true;
        }
        if (t.compare(i, 5, "false") == 0) {
            j.type = Json::Bool;
            j.b = false;
            i += 5;
            return true;
        }
        if (t.compare(i, 4, "null") == 0) {
            j.type = Json::Null;
            i += 4;
            return true;
        }
        const char *start = t.c_str() + i;
        char *end = nullptr;
        double d = std::strtod(start, &end);
        if (end == start)
            return fail("unexpected character");
        j.type = Json::Num;
        j.n = d;
        i += (size_t)(end - start);
        return true;
    }
};

bool jsonBool(const Json *j, bool def) {
    if (!j)
        return def;
    if (j->type == Json::Bool)
        return j->b;
    if (j->type == Json::Num)
        return j->n != 0;
    return def;
}
int jsonInt(const Json *j, int def) {
    return (j && j->type == Json::Num) ? (int)j->n : def;
}
std::string jsonStr(const Json *j, const std::string &def) {
    return (j && j->type == Json::Str) ? j->s : def;
}
} // namespace

// ================================================================ Config
void Config::fillDefaults() {
    chests.clear();
    auto add = [&](int id, AreaId a, bool fin, const char *title, const char *msg) {
        ChestDef d;
        d.id = id;
        d.area = a;
        d.final = fin;
        d.title = title;
        d.message = msg;
        chests.push_back(d);
    };
    add(1, AreaId::Forest, false, "A Forest Wish", "May this year be as green and full of life as the forest.");
    add(2, AreaId::Lake, false, "A Lake Wish", "May every day be calm, clear and bright.");
    add(3, AreaId::Garden, false, "A Garden Wish", "May good things bloom wherever you walk.");
    add(4, AreaId::Castle, false, "A Castle Memory", "Here is to the memories we have made together.");
    add(5, AreaId::Final, true, "Happy Birthday!", "You found everything. Thank you for being you.");
}

bool Config::load(const std::string &path) {
    fillDefaults();
    std::string text;
    if (!util::readFile(path, text)) {
        Log::warn("No %s found - using the built-in sample adventure",
                  path.c_str());
        return false;
    }
    // Skip a UTF-8 byte-order mark if an editor added one.
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        text.erase(0, 3);
    Json root;
    Parser p(text);
    if (!p.value(root) || root.type != Json::Obj) {
        Log::error("Could not read %s: %s - using defaults", path.c_str(),
                   p.err.empty() ? "top level must be an object" : p.err.c_str());
        return false;
    }
    name = jsonStr(root.get("name"), name);
    companion = jsonStr(root.get("companion"), companion);
    title = jsonStr(root.get("title"), title);
    finalMessage = jsonStr(root.get("final_message"), finalMessage);
    age = jsonInt(root.get("age"), age);
    guided = jsonBool(root.get("guided"), guided);

    if (const Json *cs = root.get("chests")) {
        if (cs->type == Json::Arr) {
            std::vector<ChestDef> list;
            int nextId = 1;
            for (const Json &c : cs->a) {
                if (c.type != Json::Obj)
                    continue;
                ChestDef d;
                d.id = jsonInt(c.get("id"), nextId);
                nextId = std::max(nextId, d.id) + 1;
                std::string area = jsonStr(c.get("area"), "forest");
                if (!parseArea(area, d.area))
                    Log::warn("Chest %d has unknown area '%s' - using forest", d.id, area.c_str());
                d.final = jsonBool(c.get("final"), false);
                d.title = jsonStr(c.get("title"), "");
                d.message = jsonStr(c.get("message"), "");
                d.photo = jsonStr(c.get("photo"), "");
                list.push_back(d);
            }
            if (!list.empty()) {
                // Exactly one chest should be the final one: keep the first marked
                // one, or promote the last chest if none was marked.
                int finals = 0;
                for (auto &d : list)
                    if (d.final && ++finals > 1)
                        d.final = false;
                if (finals == 0)
                    list.back().final = true;
                chests = std::move(list);
            }
        }
    }
    Log::info("Config loaded: %s, %zu chests", name.c_str(), chests.size());
    return true;
}

// ================================================================ GameState
void GameState::reset(uint32_t newSeed) {
    *this = GameState();
    seed = newSeed ? newSeed : 1;
}

// Save format: one "key value" pair per line. Repeated keys build up the sets.
bool GameState::save(const std::string &path) const {
    std::ostringstream o;
    o << "version 1\n";
    o << "seed " << seed << "\n";
    o << "pos " << playerPos.x << " " << playerPos.y << "\n";
    o << "facing " << facing << "\n";
    o << "flowers " << flowerProgress << "\n";
    o << "time " << playTime << "\n";
    for (auto &s : shards)
        o << "shard " << s << "\n";
    for (auto &k : keys)
        o << "key " << k << "\n";
    for (int c : chestsOpened)
        o << "chest " << c << "\n";
    for (auto &f : flags)
        o << "flag " << f << "\n";
    return util::writeFile(path, o.str());
}

bool GameState::load(const std::string &path) {
    std::string text;
    if (!util::readFile(path, text))
        return false;
    GameState g;
    bool sawVersion = false;
    for (const std::string &raw : util::splitLines(text)) {
        std::string line = util::trim(raw);
        if (line.empty())
            continue;
        size_t sp = line.find(' ');
        std::string k = line.substr(0, sp);
        std::string v = sp == std::string::npos ? "" : util::trim(line.substr(sp + 1));
        if (k == "version")
            sawVersion = true;
        else if (k == "seed")
            g.seed = (uint32_t)std::strtoul(v.c_str(), nullptr, 10);
        else if (k == "pos") {
            std::istringstream is(v);
            is >> g.playerPos.x >> g.playerPos.y;
        } else if (k == "facing")
            g.facing = std::max(0, std::min(3, std::atoi(v.c_str())));
        else if (k == "flowers")
            g.flowerProgress = std::max(0, std::min(4, std::atoi(v.c_str())));
        else if (k == "time")
            g.playTime = std::atof(v.c_str());
        else if (k == "shard")
            g.shards.insert(v);
        else if (k == "key")
            g.keys.insert(v);
        else if (k == "chest")
            g.chestsOpened.insert(std::atoi(v.c_str()));
        else if (k == "flag")
            g.flags.insert(v);
    }
    if (!sawVersion) {
        Log::warn("Save file %s looks damaged - ignoring it", path.c_str());
        return false;
    }
    if (g.seed == 0)
        g.seed = 1;
    *this = std::move(g);
    return true;
}

} // namespace bd
