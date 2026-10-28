#pragma once
// Wczytywanie makr: GDR2 (libGDR), GDR1 (msgpack), JSON / MHR (JSON).
// Format jest rozpoznawany po zawartosci pliku, nie po rozszerzeniu.

#include "Tree.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#ifndef AM_NO_GDR
#include <gdr/gdr.hpp>
#endif

namespace am {

struct Input {
    uint64_t frame = 0;
    uint8_t button = 1; // 1 = jump, 2 = left, 3 = right
    bool player2 = false;
    bool down = false;
};

struct Macro {
    std::vector<Input> inputs;
    double framerate = 240.0;
    std::string botName;
};

constexpr size_t kMaxInputs = 4'000'000;

// Zostawia tylko prawdziwe zmiany stanu przycisku (pomija powtorne "wcisniety" przy juz wcisnietym
// i "puszczony" przy puszczonym). Wymaga posortowanych wejsc.
inline void keepTransitionsOnly(Macro& m) {
    std::map<int, bool> held;
    std::vector<Input> out;
    out.reserve(m.inputs.size());
    for (auto const& in : m.inputs) {
        int key = (in.player2 ? 4 : 0) + in.button;
        bool& h = held[key];
        if (in.down == h) continue;
        h = in.down;
        out.push_back(in);
    }
    m.inputs = std::move(out);
}

inline double sanitizeRate(double rate) {
    if (!std::isfinite(rate) || rate < 30.0 || rate > 10000.0) return 240.0;
    return rate;
}

inline void sortInputs(Macro& m) {
    std::stable_sort(m.inputs.begin(), m.inputs.end(),
                     [](Input const& a, Input const& b) { return a.frame < b.frame; });
}

#ifndef AM_NO_GDR
struct GdrReplay : gdr::Replay<GdrReplay, gdr::Input<>> {
    GdrReplay() = default;
};

inline bool parseGdr2(std::vector<uint8_t> const& data, Macro& out, std::string& err) {
    std::vector<uint8_t> copy(data);
    try {
        auto imported = GdrReplay::importData(std::span<uint8_t>(copy));
        if (imported.isErr()) {
            err = std::string("GDR2: ") + std::string(imported.unwrapErr());
            return false;
        }
        GdrReplay replay = std::move(imported).unwrap();
        if (replay.inputs.empty()) { err = "GDR2: brak wejsc"; return false; }
        out.inputs.reserve(replay.inputs.size());
        for (auto const& in : replay.inputs) {
            Input i;
            i.frame = in.frame;
            i.button = (in.button >= 1 && in.button <= 3) ? uint8_t(in.button) : uint8_t(1);
            i.player2 = in.player2;
            i.down = in.down;
            out.inputs.push_back(i);
        }
        out.framerate = sanitizeRate(replay.framerate);
        out.botName = replay.botInfo.name.empty() ? "GDR2" : replay.botInfo.name;
        return true;
    } catch (std::exception const& e) {
        err = std::string("GDR2 wyjatek: ") + e.what();
        return false;
    } catch (...) {
        err = "GDR2: nieznany wyjatek";
        return false;
    }
}
#endif

// Makra JSON/MHR/GDR1 trafiaja do wspolnego ekstraktora (rozne boty nazywaja pola roznie).
inline bool macroFromTree(Node const& root, Macro& out, std::string& err) {
    Node const* arr = nullptr;
    if (root.t == Node::T::Arr) {
        arr = &root;
    } else if (auto* f = root.first({"inputs", "macro", "events", "actions", "clicks", "replay", "data", "frames"})) {
        if (f->t == Node::T::Arr) arr = f;
    }
    if (!arr) { err = "brak listy wejsc"; return false; }
    if (arr->a.empty() || arr->a.size() > kMaxInputs) { err = "zla liczba wejsc"; return false; }

    static constexpr std::initializer_list<char const*> kDownKeys = {"down", "hold", "isDown", "pressed", "press", "holding", "state"};
    bool sawDownKey = false;
    for (auto const& e : arr->a) {
        if (e.t == Node::T::Map && e.first(kDownKeys)) { sawDownKey = true; break; }
    }
    for (auto const& e : arr->a) {
        if (e.t != Node::T::Map) continue;
        if (sawDownKey && !e.first(kDownKeys)) continue; // wpis bez flagi wcisniecia to nie input
        auto* fr = e.first({"frame", "f", "step", "tick"});
        if (!fr) continue;
        double fv = fr->num(-1.0);
        if (!std::isfinite(fv) || fv < 0.0) continue;

        Input in;
        in.frame = uint64_t(fv);
        if (auto* d = e.first(kDownKeys)) in.down = d->flag(false);
        if (auto* p = e.first({"player2", "p2", "isPlayer2", "2p", "second", "player_2"})) {
            in.player2 = p->flag(false);
        } else if (auto* p1 = e.first({"player1", "p1", "isPlayer1"})) {
            in.player2 = !p1->flag(true);
        }
        if (auto* b = e.first({"button", "btn", "key"})) {
            long r = std::lround(b->num(1.0));
            in.button = (r >= 1 && r <= 3) ? uint8_t(r) : uint8_t(1);
        }
        out.inputs.push_back(in);
    }
    if (out.inputs.empty()) { err = "brak uzytecznych wejsc"; return false; }

    // Brak jakiejkolwiek flagi wcisniecia/puszczenia -> zakladamy, ze zdarzenia naprzemiennie
    // wciskaja i puszczaja przycisk (kazdy gracz/przycisk osobno).
    if (!sawDownKey) {
        sortInputs(out);
        std::map<int, bool> held;
        for (auto& in : out.inputs) {
            int key = (in.player2 ? 4 : 0) + in.button;
            in.down = !held[key];
            held[key] = in.down;
        }
    }

    double rate = 0.0;
    if (auto* f = root.first({"framerate", "fps", "tps", "tickRate", "frameRate", "FPS"})) {
        rate = f->num(0.0);
    } else if (auto* meta = root.first({"meta", "info", "header", "settings"})) {
        if (auto* f2 = meta->first({"framerate", "fps", "tps", "tickRate", "frameRate", "FPS"})) rate = f2->num(0.0);
    }
    out.framerate = sanitizeRate(rate);
    out.botName = "JSON";
    return true;
}


// Stary binarny format zBot (.zbf): naglowek 8 bajtow (float: czas klatki + 4 bajty),
// potem rekordy po 6 bajtow: u32 klatka, bajt '0'/'1' (wcisniety), bajt '0'/'1' (gracz; '1' = P1).
inline bool looksLikeZbf(std::vector<uint8_t> const& d) {
    if (d.size() <= 8 || (d.size() - 8) % 6 != 0) return false;
    float delta;
    std::memcpy(&delta, d.data(), 4);
    if (!std::isfinite(delta) || delta <= 0.f) return false;
    double rate = 1.0 / double(delta);
    if (rate < 30.0 || rate > 10000.0) return false;
    size_t records = (d.size() - 8) / 6;
    size_t probe = std::min<size_t>(records, 8);
    for (size_t i = 0; i < probe; ++i) {
        size_t off = 8 + i * 6;
        if ((d[off + 4] != '0' && d[off + 4] != '1') || (d[off + 5] != '0' && d[off + 5] != '1')) return false;
    }
    return probe > 0;
}

inline bool parseZbf(std::vector<uint8_t> const& d, Macro& out, std::string& err) {
    float delta;
    std::memcpy(&delta, d.data(), 4);
    out.framerate = sanitizeRate(1.0 / double(delta));
    out.botName = "zBot";
    size_t records = (d.size() - 8) / 6;
    if (records == 0 || records > kMaxInputs) { err = "zbf: zla liczba rekordow"; return false; }
    out.inputs.reserve(records);
    for (size_t i = 0; i < records; ++i) {
        size_t off = 8 + i * 6;
        uint32_t frame;
        std::memcpy(&frame, d.data() + off, 4);
        Input in;
        in.frame = frame;
        in.down = d[off + 4] == '1';
        in.player2 = d[off + 5] == '0';
        in.button = 1;
        out.inputs.push_back(in);
    }
    return true;
}

inline std::optional<Macro> parseMacro(std::vector<uint8_t> const& data, std::string& err) {
    Macro m;
    if (data.size() < 4) { err = "plik za maly"; return std::nullopt; }

    // GDR2: magic "GDR"
    if (data[0] == 'G' && data[1] == 'D' && data[2] == 'R') {
#ifndef AM_NO_GDR
        if (!parseGdr2(data, m, err)) return std::nullopt;
        sortInputs(m);
        return m;
#else
        err = "GDR2 wylaczone w tym buildzie";
        return std::nullopt;
#endif
    }

    // JSON (json, mhr, czesc makr gdr)
    size_t i = 0;
    while (i < data.size() && std::isspace(data[i])) ++i;
    if (i < data.size() && (data[i] == '{' || data[i] == '[')) {
        Node root;
        JsonParser p(std::string_view(reinterpret_cast<char const*>(data.data()), data.size()));
        if (!p.parse(root)) { err = "niepoprawny JSON"; return std::nullopt; }
        if (!macroFromTree(root, m, err)) return std::nullopt;
        sortInputs(m);
        return m;
    }

    // zBot (.zbf)
    if (looksLikeZbf(data)) {
        if (!parseZbf(data, m, err)) return std::nullopt;
        sortInputs(m);
        return m;
    }

    // GDR1 = msgpack
    Node root;
    MsgpackDecoder d(data.data(), data.size());
    if (d.parse(root) && macroFromTree(root, m, err)) {
        m.botName = "GDR1";
        sortInputs(m);
        return m;
    }
    if (err.empty()) err = "nierozpoznany format makra";
    return std::nullopt;
}

} // namespace am
