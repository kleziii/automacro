#pragma once
// Minimalne drzewo wartosci + parser JSON + dekoder MessagePack.
// Nie zalezy od Geode ani matjson, dzieki czemu parsowanie makr jest przenosne.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace am {

struct Node {
    enum class T { Nil, Bool, Num, Str, Arr, Map };
    T t = T::Nil;
    bool b = false;
    double n = 0.0;
    std::string s;
    std::vector<Node> a;            // tablica
    std::vector<std::string> keys;  // mapa: klucze
    std::vector<Node> vals;         // mapa: wartosci

    Node const* get(char const* key) const {
        if (t != T::Map) return nullptr;
        for (size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key) return &vals[i];
        }
        return nullptr;
    }

    Node const* first(std::initializer_list<char const*> ks) const {
        for (char const* k : ks) {
            if (auto* p = get(k)) return p;
        }
        return nullptr;
    }

    double num(double fallback) const {
        if (t == T::Num) return n;
        if (t == T::Bool) return b ? 1.0 : 0.0;
        if (t == T::Str && !s.empty()) {
            char* end = nullptr;
            double d = std::strtod(s.c_str(), &end);
            if (end != s.c_str() && std::isfinite(d)) return d;
        }
        return fallback;
    }

    bool flag(bool fallback) const {
        if (t == T::Bool) return b;
        if (t == T::Num) return n != 0.0;
        if (t == T::Str) return s == "true" || s == "True" || s == "1" || s == "yes";
        return fallback;
    }

    std::string text(std::string const& fallback) const {
        return t == T::Str ? s : fallback;
    }
};

constexpr int kMaxDepth = 64;

// ------------------------------------------------------------------
// JSON
// ------------------------------------------------------------------
class JsonParser {
public:
    explicit JsonParser(std::string_view text) : m_s(text) {}

    bool parse(Node& out) {
        skipWs();
        if (!value(out, 0)) return false;
        skipWs();
        return m_i == m_s.size();
    }

private:
    std::string_view m_s;
    size_t m_i = 0;

    void skipWs() {
        while (m_i < m_s.size() && (m_s[m_i] == ' ' || m_s[m_i] == '\t' || m_s[m_i] == '\n' || m_s[m_i] == '\r')) ++m_i;
    }

    bool lit(char const* word) {
        size_t len = std::strlen(word);
        if (m_s.compare(m_i, len, word) != 0) return false;
        m_i += len;
        return true;
    }

    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(char(cp));
        } else if (cp < 0x800) {
            out.push_back(char(0xC0 | (cp >> 6)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(char(0xE0 | (cp >> 12)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        }
    }

    bool str(std::string& out) {
        if (m_i >= m_s.size() || m_s[m_i] != '"') return false;
        ++m_i;
        while (m_i < m_s.size()) {
            char c = m_s[m_i++];
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (m_i >= m_s.size()) return false;
            char e = m_s[m_i++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (m_i + 4 > m_s.size()) return false;
                    uint32_t cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = m_s[m_i++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= uint32_t(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= uint32_t(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= uint32_t(h - 'A' + 10);
                        else return false;
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool value(Node& out, int depth) {
        if (depth > kMaxDepth) return false;
        skipWs();
        if (m_i >= m_s.size()) return false;
        char c = m_s[m_i];

        if (c == '{') {
            ++m_i;
            out.t = Node::T::Map;
            skipWs();
            if (m_i < m_s.size() && m_s[m_i] == '}') { ++m_i; return true; }
            while (true) {
                skipWs();
                std::string key;
                if (!str(key)) return false;
                skipWs();
                if (m_i >= m_s.size() || m_s[m_i] != ':') return false;
                ++m_i;
                Node v;
                if (!value(v, depth + 1)) return false;
                out.keys.push_back(std::move(key));
                out.vals.push_back(std::move(v));
                skipWs();
                if (m_i >= m_s.size()) return false;
                if (m_s[m_i] == ',') { ++m_i; continue; }
                if (m_s[m_i] == '}') { ++m_i; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++m_i;
            out.t = Node::T::Arr;
            skipWs();
            if (m_i < m_s.size() && m_s[m_i] == ']') { ++m_i; return true; }
            while (true) {
                Node v;
                if (!value(v, depth + 1)) return false;
                out.a.push_back(std::move(v));
                skipWs();
                if (m_i >= m_s.size()) return false;
                if (m_s[m_i] == ',') { ++m_i; continue; }
                if (m_s[m_i] == ']') { ++m_i; return true; }
                return false;
            }
        }
        if (c == '"') {
            out.t = Node::T::Str;
            return str(out.s);
        }
        if (c == 't') { out.t = Node::T::Bool; out.b = true; return lit("true"); }
        if (c == 'f') { out.t = Node::T::Bool; out.b = false; return lit("false"); }
        if (c == 'n') { out.t = Node::T::Nil; return lit("null"); }

        // liczba
        size_t start = m_i;
        while (m_i < m_s.size()) {
            char d = m_s[m_i];
            if ((d >= '0' && d <= '9') || d == '-' || d == '+' || d == '.' || d == 'e' || d == 'E') ++m_i;
            else break;
        }
        if (m_i == start) return false;
        std::string numText(m_s.substr(start, m_i - start));
        char* end = nullptr;
        double d = std::strtod(numText.c_str(), &end);
        if (end == numText.c_str() || !std::isfinite(d)) return false;
        out.t = Node::T::Num;
        out.n = d;
        return true;
    }
};

// ------------------------------------------------------------------
// MessagePack (stary format GDR1 jest zapisany jako msgpack)
// ------------------------------------------------------------------
class MsgpackDecoder {
public:
    MsgpackDecoder(uint8_t const* data, size_t size) : m_d(data), m_n(size) {}

    bool parse(Node& out) {
        if (!value(out, 0)) return false;
        return m_i == m_n; // cale dane musza zostac zuzyte
    }

private:
    uint8_t const* m_d;
    size_t m_n;
    size_t m_i = 0;

    bool have(size_t k) const { return k <= m_n - m_i; }

    bool uint(size_t width, uint64_t& out) {
        if (!have(width)) return false;
        uint64_t v = 0;
        for (size_t k = 0; k < width; ++k) v = (v << 8) | m_d[m_i + k];
        m_i += width;
        out = v;
        return true;
    }

    bool skip(uint64_t k) {
        if (!have(size_t(k)) || k > m_n) return false;
        m_i += size_t(k);
        return true;
    }

    bool readStr(uint64_t len, std::string& out) {
        if (len > m_n || !have(size_t(len))) return false;
        out.assign(reinterpret_cast<char const*>(m_d + m_i), size_t(len));
        m_i += size_t(len);
        return true;
    }

    bool array(uint64_t count, Node& out, int depth) {
        if (count > m_n - m_i) return false; // kazdy element zajmuje >= 1 bajt
        out.t = Node::T::Arr;
        out.a.reserve(size_t(count));
        for (uint64_t k = 0; k < count; ++k) {
            Node v;
            if (!value(v, depth + 1)) return false;
            out.a.push_back(std::move(v));
        }
        return true;
    }

    bool map(uint64_t count, Node& out, int depth) {
        if (count > (m_n - m_i) / 2) return false;
        out.t = Node::T::Map;
        for (uint64_t k = 0; k < count; ++k) {
            Node key, v;
            if (!value(key, depth + 1)) return false;
            if (!value(v, depth + 1)) return false;
            std::string ks;
            if (key.t == Node::T::Str) ks = key.s;
            else if (key.t == Node::T::Num) ks = std::to_string(int64_t(key.n));
            else return false;
            out.keys.push_back(std::move(ks));
            out.vals.push_back(std::move(v));
        }
        return true;
    }

    bool num(double v, Node& out) {
        out.t = Node::T::Num;
        out.n = v;
        return true;
    }

    bool value(Node& out, int depth) {
        if (depth > kMaxDepth || !have(1)) return false;
        uint8_t b = m_d[m_i++];
        uint64_t v = 0;

        if (b <= 0x7F) return num(double(b), out);
        if (b >= 0xE0) return num(double(int8_t(b)), out);
        if (b >= 0x80 && b <= 0x8F) return map(b & 0x0F, out, depth);
        if (b >= 0x90 && b <= 0x9F) return array(b & 0x0F, out, depth);
        if (b >= 0xA0 && b <= 0xBF) { out.t = Node::T::Str; return readStr(b & 0x1F, out.s); }

        switch (b) {
            case 0xC0: out.t = Node::T::Nil; return true;
            case 0xC2: out.t = Node::T::Bool; out.b = false; return true;
            case 0xC3: out.t = Node::T::Bool; out.b = true; return true;
            case 0xC4: case 0xC5: case 0xC6: { // bin
                size_t w = b == 0xC4 ? 1 : b == 0xC5 ? 2 : 4;
                if (!uint(w, v)) return false;
                return skip(v);
            }
            case 0xC7: case 0xC8: case 0xC9: { // ext
                size_t w = b == 0xC7 ? 1 : b == 0xC8 ? 2 : 4;
                if (!uint(w, v)) return false;
                return skip(v + 1);
            }
            case 0xCA: {
                if (!uint(4, v)) return false;
                uint32_t bits = uint32_t(v);
                float f;
                std::memcpy(&f, &bits, 4);
                return num(double(f), out);
            }
            case 0xCB: {
                if (!uint(8, v)) return false;
                double d;
                std::memcpy(&d, &v, 8);
                return num(d, out);
            }
            case 0xCC: if (!uint(1, v)) return false; return num(double(v), out);
            case 0xCD: if (!uint(2, v)) return false; return num(double(v), out);
            case 0xCE: if (!uint(4, v)) return false; return num(double(v), out);
            case 0xCF: if (!uint(8, v)) return false; return num(double(v), out);
            case 0xD0: if (!uint(1, v)) return false; return num(double(int8_t(v)), out);
            case 0xD1: if (!uint(2, v)) return false; return num(double(int16_t(v)), out);
            case 0xD2: if (!uint(4, v)) return false; return num(double(int32_t(v)), out);
            case 0xD3: if (!uint(8, v)) return false; return num(double(int64_t(v)), out);
            case 0xD4: return skip(2);
            case 0xD5: return skip(3);
            case 0xD6: return skip(5);
            case 0xD7: return skip(9);
            case 0xD8: return skip(17);
            case 0xD9: if (!uint(1, v)) return false; out.t = Node::T::Str; return readStr(v, out.s);
            case 0xDA: if (!uint(2, v)) return false; out.t = Node::T::Str; return readStr(v, out.s);
            case 0xDB: if (!uint(4, v)) return false; out.t = Node::T::Str; return readStr(v, out.s);
            case 0xDC: if (!uint(2, v)) return false; return array(v, out, depth);
            case 0xDD: if (!uint(4, v)) return false; return array(v, out, depth);
            case 0xDE: if (!uint(2, v)) return false; return map(v, out, depth);
            case 0xDF: if (!uint(4, v)) return false; return map(v, out, depth);
            default: return false;
        }
    }
};

} // namespace am
