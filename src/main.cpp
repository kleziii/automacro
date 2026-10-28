// AutoMacro - wykrywa level, pobiera makro (Hyperbolus / wlasne zrodla) i odtwarza je.
// Geode SDK 5.x, Geometry Dash 2.2081

#include "Bot.hpp"
#include "MacroParse.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <functional>
#include <memory>
#include <optional>
#include <set>

using namespace geode::prelude;

namespace am {

constexpr size_t MAX_MACRO_BYTES = 16 * 1024 * 1024;
constexpr char const* kHyperbolusApi = "https://hyperbolus.net/api/macros?level_id=";
constexpr char const* kHyperbolusTag = "@hyperbolus";
constexpr int kRatePerMinute = 30;      // Hyperbolus pozwala na 60/min, trzymamy sie polowy
constexpr size_t kMaxCandidates = 5;    // max prob pobrania na jeden level

// ------------------------------------------------------------------
// Stan
// ------------------------------------------------------------------
struct State {
    Bot bot;                      // odtwarzacz makr (port z Eclipse Menu)
    // Wykrywanie skoku licznika klatek: w niektorych wersjach GD m_currentProgress rosnie o 2 (lub wiecej)
    // na jeden krok fizyki, a makra liczone sa co 1 krok.
    int64_t lastBeat = -1;
    int64_t lastProgress = -1;
    int strideSamples = 0;
    int stride = 0;               // 0 = jeszcze nie wykryto
    int pendingStride = 0;
    // TPS bypass (port z Eclipse Menu: hacks/Global/TPSBypass.cpp) - TPS gry = TPS makra
    double targetTps = 240.0;     // TPS, w jakim nagrano makro
    bool tpsActive = false;       // czy w tej klatce dzialamy z niestandardowym TPS
    bool tpsHide = false;         // trwa petla dodatkowych krokow fizyki
    double extraDelta = 0.0;
    float realDelta = 0.f;
    bool injecting = false;       // true, gdy to MY wstrzykujemy przycisk (nie uzytkownik)
    bool needSync = true;         // po resecie/checkpoincie: ustaw wskaznik makra i stan przyciskow
    int generation = 0;
    size_t applied = 0;           // ile wejsc odtworzono w tym levelu
    bool announced = false;
    bool stallWarned = false;
    bool pausePending = false;    // uzytkownik spauzowal, czekamy na restart
    float pausedPercent = 0.f;
    std::set<uint32_t> missing; // levele, dla ktorych w tej sesji nic nie znaleziono
    std::deque<std::chrono::steady_clock::time_point> requests;
};

static State& st() {
    static State s;
    return s;
}

// Click Between Frames podmienia obsluge wejscia i psuje wstrzykiwane wcisniecia/puszczenia.
// Eclipse Menu wylacza go na czas dzialania bota (ustawienie "soft-toggle") - robimy tak samo.
static bool s_cbfTouched = false;
static bool s_cbfPrev = false;

static void cbfOff() {
    auto* cbf = Loader::get()->getLoadedMod("syzzi.click_between_frames");
    if (!cbf || s_cbfTouched) return;
    s_cbfPrev = cbf->getSettingValue<bool>("soft-toggle");
    cbf->setSettingValue<bool>("soft-toggle", true);
    s_cbfTouched = true;
    log::info("AutoMacro: wylaczono Click Between Frames na czas makra");
}

static void cbfRestore() {
    if (!s_cbfTouched) return;
    s_cbfTouched = false;
    if (auto* cbf = Loader::get()->getLoadedMod("syzzi.click_between_frames")) {
        cbf->setSettingValue<bool>("soft-toggle", s_cbfPrev);
    }
}

static void notify(std::string const& msg, NotificationIcon icon = NotificationIcon::Info) {
    if (!Mod::get()->getSettingValue<bool>("notifications")) return;
    Notification::create(msg, icon)->show();
}

// ------------------------------------------------------------------
// Limit zapytan (po stronie klienta)
// ------------------------------------------------------------------
static bool rateAllows() {
    auto& q = st().requests;
    auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(60);
    while (!q.empty() && q.front() < cutoff) q.pop_front();
    return int(q.size()) < kRatePerMinute;
}

static void noteRequest() {
    st().requests.push_back(std::chrono::steady_clock::now());
}

static web::WebRequest baseRequest() {
    web::WebRequest req;
    req.userAgent(fmt::format("AutoMacro-GeodeMod/{}", Mod::get()->getVersion().toNonVString()));
    req.timeout(std::chrono::seconds(20));
    return req;
}

// ------------------------------------------------------------------
// Cache
// ------------------------------------------------------------------
static std::filesystem::path cacheDir() {
    return Mod::get()->getSaveDir() / "macros";
}

static void saveCache(uint32_t id, std::vector<uint8_t> const& data) {
    std::error_code ec;
    std::filesystem::create_directories(cacheDir(), ec);
    std::ofstream f(cacheDir() / fmt::format("{}.macro", id), std::ios::binary | std::ios::trunc);
    if (f) f.write(reinterpret_cast<char const*>(data.data()), std::streamsize(data.size()));
}

// Wczytuje makro z bajtow. Zwraca true, jesli sie udalo.
static bool useMacroData(uint32_t id, std::vector<uint8_t> const& data, bool fromNetwork, std::string const& origin) {
    std::string err;
    auto parsed = parseMacro(data, err);
    if (!parsed || parsed->inputs.empty()) {
        log::warn("AutoMacro: nie udalo sie wczytac makra z {}: {}", origin, err);
        return false;
    }
    Macro m = std::move(*parsed);

    // makra, w ktorych wszystkie wejscia sa oznaczone jako gracz 2 (np. zle odczytana flaga) -> gracz 1
    bool anyP1 = std::any_of(m.inputs.begin(), m.inputs.end(), [](Input const& i) { return !i.player2; });
    if (!anyP1) for (auto& i : m.inputs) i.player2 = false;
    size_t presses = size_t(std::count_if(m.inputs.begin(), m.inputs.end(), [](Input const& i) { return i.down; }));

    // przelicz klatki na 240 TPS (standardowa fizyka GD)
    // Auto-TPS: klatki zostaja surowe, a TPS gry ustawiamy na TPS makra (hook update nizej).
    bool autoTps = Mod::get()->getSettingValue<bool>("auto-tps");
    double macroTps = (m.framerate >= 1.0 && m.framerate <= 100000.0) ? m.framerate : 240.0;
    double scale = autoTps ? 1.0 : 240.0 / macroTps;
    bool rescaled = std::abs(scale - 1.0) > 1e-6;
    if (rescaled) {
        for (auto& in : m.inputs) in.frame = uint64_t(std::llround(double(in.frame) * scale));
    }
    sortInputs(m);
    presses = size_t(std::count_if(m.inputs.begin(), m.inputs.end(), [](Input const& i) { return i.down; }));
    {
        std::string head;
        for (size_t k = 0; k < std::min<size_t>(m.inputs.size(), 12); ++k) {
            head += fmt::format(" [{} {} p{}]", m.inputs[k].frame, m.inputs[k].down ? "DOWN" : "UP", m.inputs[k].player2 ? 2 : 1);
        }
        log::info("AutoMacro: format/bot={}, pierwsze zdarzenia:{}", m.botName, head);
    }

    if (fromNetwork) saveCache(id, data);

    auto& s = st();
    size_t count = m.inputs.size();
    double fps = m.framerate;
    log::info("AutoMacro: wczytano makro ({}), {} wejsc, {} wcisniec, {} TPS, pierwsza klatka {}",
              origin, count, presses, fps, m.inputs.front().frame);
    cbfOff();
    s.targetTps = autoTps ? macroTps : 240.0;
    s.extraDelta = 0.0;
    s.bot.load(std::move(m));
    s.bot.setState(BotState::PLAYBACK); // automatyczne wlaczenie odtwarzania
    s.needSync = true;
    s.applied = 0;
    s.announced = false;
    s.stallWarned = false;

    if (rescaled) {
        notify(fmt::format("Makro wczytane ({} wejsc, {} wcisniec), ale nagrane w {:.0f} TPS - moze sie rozjezdzac", count, presses, fps),
               NotificationIcon::Warning);
    } else if (autoTps && std::abs(macroTps - 240.0) >= 0.5) {
        notify(fmt::format("Makro wczytane ({} wejsc, {} wcisniec), TPS gry ustawiony na {:.0f}", count, presses, macroTps),
               NotificationIcon::Success);
    } else {
        notify(fmt::format("Makro wczytane ({} wejsc, {} wcisniec)", count, presses), NotificationIcon::Success);
    }
    return true;
}

// ------------------------------------------------------------------
// Pobieranie plikow
// ------------------------------------------------------------------
static void downloadUrl(uint32_t id, int gen, std::string url, std::function<void(bool)> done) {
    if (!url.starts_with("https://")) {
        log::warn("AutoMacro: pomijam zrodlo bez https: {}", url);
        done(false);
        return;
    }
    if (!rateAllows()) {
        notify("Limit zapytan - sprobuj ponownie za chwile", NotificationIcon::Warning);
        done(false);
        return;
    }
    noteRequest();
    auto req = baseRequest();
    async::spawn(req.get(url), [id, gen, url, done](web::WebResponse res) {
        if (gen != st().generation) return; // gracz zmienil level
        if (!res.ok()) { done(false); return; }
        auto const& d = res.data();
        if (d.empty() || d.size() > MAX_MACRO_BYTES) { done(false); return; }
        std::vector<uint8_t> data(d.begin(), d.end());
        done(useMacroData(id, data, true, url));
    });
}

static void tryUrls(uint32_t id, int gen, std::shared_ptr<std::vector<std::string>> urls, size_t i,
                    std::function<void(bool)> done) {
    if (i >= urls->size()) { done(false); return; }
    downloadUrl(id, gen, (*urls)[i], [=](bool ok) {
        if (ok) done(true);
        else tryUrls(id, gen, urls, i + 1, done);
    });
}

// ------------------------------------------------------------------
// Hyperbolus: GET https://hyperbolus.net/api/macros?level_id=<ID>
// Odpowiedz: { "data": [ { "format", "approved_at", "files": [ {"url","bytes","downloads"} ] } ] }
// ------------------------------------------------------------------
struct Candidate {
    std::string format;
    std::string url;
    long long bytes = 0;
    long long downloads = 0;
    bool approved = false;
};

static std::string lowered(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// obslugiwane formaty i ich priorytet (nizsze = lepsze)
static int formatRank(std::string const& f) {
    if (f == "gdr2") return 0;
    if (f == "gdr") return 1;
    if (f == "mhr") return 2;
    if (f == "json") return 3;
    if (f == "zbot_frame") return 4;
    if (f == "zbot") return 5;
    return 99;
}

static void fetchHyperbolus(uint32_t id, int gen, std::function<void(bool)> done) {
    if (!rateAllows()) {
        notify("Limit zapytan - sprobuj ponownie za chwile", NotificationIcon::Warning);
        done(false);
        return;
    }
    noteRequest();
    auto req = baseRequest();
    req.header("Accept", "application/json");
    std::string url = kHyperbolusApi + std::to_string(id);

    async::spawn(req.get(url), [id, gen, done](web::WebResponse res) {
        if (gen != st().generation) return;
        if (!res.ok()) {
            log::warn("AutoMacro: Hyperbolus odpowiedzial kodem {}", res.code());
            done(false);
            return;
        }
        std::string body = res.string().unwrapOr("");
        Node root;
        JsonParser parser(body);
        if (body.empty() || !parser.parse(root)) { done(false); return; }

        Node const* data = root.get("data");
        if (!data || data->t != Node::T::Arr) { done(false); return; }

        std::vector<Candidate> list;
        for (auto const& entry : data->a) {
            if (list.size() >= 60) break;
            if (entry.t != Node::T::Map) continue;

            Candidate c;
            if (auto* f = entry.get("format")) c.format = lowered(f->text(""));
            if (auto* a = entry.get("approved_at")) c.approved = a->t == Node::T::Str && !a->s.empty();
            auto* files = entry.get("files");
            if (!files || files->t != Node::T::Arr || files->a.empty()) continue;
            auto const& file = files->a.front();
            if (auto* u = file.get("url")) c.url = u->text("");
            if (auto* b = file.get("bytes")) c.bytes = (long long)b->num(0);
            if (auto* d = file.get("downloads")) c.downloads = (long long)d->num(0);

            if (!c.url.starts_with("https://")) continue;
            if (c.bytes <= 0 || size_t(c.bytes) > MAX_MACRO_BYTES) continue;
            if (formatRank(c.format) >= 99) continue;
            list.push_back(std::move(c));
        }

        std::stable_sort(list.begin(), list.end(), [](Candidate const& a, Candidate const& b) {
            if (a.approved != b.approved) return a.approved;
            int ra = formatRank(a.format), rb = formatRank(b.format);
            if (ra != rb) return ra < rb;
            return a.downloads > b.downloads;
        });

        auto urls = std::make_shared<std::vector<std::string>>();
        for (auto const& c : list) {
            if (urls->size() >= kMaxCandidates) break;
            urls->push_back(c.url);
        }
        log::info("AutoMacro: Hyperbolus - {} uzytecznych makr dla levelu {}", urls->size(), id);
        tryUrls(id, gen, urls, 0, done);
    });
}

// ------------------------------------------------------------------
// Wlasne zrodla: szablon URL z {id} albo indeks JSON {"<id>": "url" | ["url", ...]}
// ------------------------------------------------------------------
static void fetchIndex(uint32_t id, int gen, std::string indexUrl, std::function<void(bool)> done) {
    if (!indexUrl.starts_with("https://") || !rateAllows()) { done(false); return; }
    noteRequest();
    auto req = baseRequest();
    async::spawn(req.get(indexUrl), [id, gen, done](web::WebResponse res) {
        if (gen != st().generation) return;
        if (!res.ok()) { done(false); return; }
        std::string body = res.string().unwrapOr("");
        Node root;
        JsonParser parser(body);
        if (body.empty() || !parser.parse(root)) { done(false); return; }

        auto urls = std::make_shared<std::vector<std::string>>();
        if (auto* e = root.get(std::to_string(id).c_str())) {
            if (e->t == Node::T::Str) urls->push_back(e->s);
            else if (e->t == Node::T::Arr) {
                for (auto const& x : e->a) if (x.t == Node::T::Str) urls->push_back(x.s);
            }
        }
        tryUrls(id, gen, urls, 0, done);
    });
}

static void trySources(uint32_t id, int gen, std::shared_ptr<std::vector<std::string>> srcs, size_t i) {
    if (i >= srcs->size()) {
        st().missing.insert(id);
        notify(fmt::format("Nie znaleziono makra dla levelu {}", id), NotificationIcon::Warning);
        return;
    }
    std::string s = (*srcs)[i];
    auto next = [=](bool ok) {
        if (!ok) trySources(id, gen, srcs, i + 1);
    };

    if (s == kHyperbolusTag) {
        fetchHyperbolus(id, gen, next);
        return;
    }
    auto pos = s.find("{id}");
    if (pos != std::string::npos) {
        std::string idStr = std::to_string(id);
        while ((pos = s.find("{id}")) != std::string::npos) s.replace(pos, 4, idStr);
        auto urls = std::make_shared<std::vector<std::string>>(std::vector<std::string>{s});
        tryUrls(id, gen, urls, 0, next);
    } else {
        fetchIndex(id, gen, s, next);
    }
}

static std::vector<std::string> splitSources(std::string const& in) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : in) {
        if (c == ' ' || c == ';' || c == '\n' || c == '\t' || c == '\r') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// ------------------------------------------------------------------
// Wejscie do levelu
// ------------------------------------------------------------------
static void onLevelEnter(GJGameLevel* level) {
    auto& s = st();
    s.generation++;
    s.bot.unload();
    s.needSync = true;
    s.applied = 0;
    s.announced = false;
    s.stallWarned = false;
    s.pausePending = false;

    if (!level) return;
    if (!Mod::get()->getSettingValue<bool>("auto-detect")) return;

    int rawId = level->m_levelID.value();
    if (rawId <= 0) return; // level lokalny / z edytora
    uint32_t id = uint32_t(rawId);
    int gen = s.generation;

    // 1) pamiec podreczna / pliki wrzucone recznie do folderu makr
    for (char const* ext : {"macro", "gdr2", "gdr", "json"}) {
        auto path = cacheDir() / fmt::format("{}.{}", id, ext);
        std::ifstream f(path, std::ios::binary);
        if (!f) continue;
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!data.empty() && data.size() <= MAX_MACRO_BYTES && useMacroData(id, data, false, path.string())) return;
    }

    // 2) zrodla sieciowe
    auto srcs = std::make_shared<std::vector<std::string>>();
    if (Mod::get()->getSettingValue<bool>("hyperbolus")) srcs->push_back(kHyperbolusTag);
    for (auto& x : splitSources(Mod::get()->getSettingValue<std::string>("sources"))) srcs->push_back(x);

    if (srcs->empty()) {
        notify("Brak makra lokalnie i brak wlaczonych zrodel w ustawieniach.", NotificationIcon::Warning);
        return;
    }
    if (s.missing.count(id)) {
        notify(fmt::format("Brak makra dla levelu {} (sprawdzone wczesniej w tej sesji)", id), NotificationIcon::Warning);
        return;
    }
    notify(fmt::format("Szukam makra dla levelu {}...", id));
    trySources(id, gen, srcs, 0);
}

// ------------------------------------------------------------------
// TPS: gra dziala w takim TPS, w jakim nagrano makro (bez safe mode)
// ------------------------------------------------------------------
static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Zwraca TPS do uzycia albo 0, gdy gra ma dzialac normalnie (240).
static float activeTps(GJBaseGameLayer* layer) {
    if (!typeinfo_cast<PlayLayer*>(layer)) return 0.f;
    auto& s = st();
    if (!s.bot.isPlayback()) return 0.f;
    if (!Mod::get()->getSettingValue<bool>("autoplay")) return 0.f;
    if (!Mod::get()->getSettingValue<bool>("auto-tps")) return 0.f;
    if (std::abs(s.targetTps - 240.0) < 0.5) return 0.f;
    return float(s.targetTps);
}

// Procent postepu liczony z czasu (poprawny przy TPS != 240); jak Eclipse utils::getActualProgress.
static float actualProgress(GJBaseGameLayer* game) {
    float percent;
    if (game->m_level->m_timestamp > 0) {
        percent = float(game->m_gameState.m_levelTime * 240.f) / game->m_level->m_timestamp * 100.f;
    } else {
        percent = game->m_player1->getPositionX() / game->m_levelLength * 100.f;
    }
    return std::clamp(percent, 0.f, 100.f);
}

// ------------------------------------------------------------------
// Odtwarzanie (logika w hooku GJBaseGameLayer::processCommands, jak w Eclipse Menu)
// ------------------------------------------------------------------
static bool macroActive(GJBaseGameLayer* layer) {
    if (!typeinfo_cast<PlayLayer*>(layer)) return false;
    if (!st().bot.isPlayback()) return false;
    return Mod::get()->getSettingValue<bool>("autoplay");
}

// Blokujemy klikniecia gracza, dopoki makro ma jeszcze wejscia do odtworzenia.
static bool blockingUserInput(GJBaseGameLayer* layer) {
    if (!macroActive(layer)) return false;
    if (!Mod::get()->getSettingValue<bool>("block-input")) return false;
    return st().bot.hasPending();
}

// ------------------------------------------------------------------
// Pauza: zapamietaj % i zrob checkpoint, zeby restart zaczynal od tego miejsca
// ------------------------------------------------------------------
static void onPause() {
    auto* pl = PlayLayer::get();
    auto& s = st();
    if (!pl || !s.bot.isPlayback()) return;

    float pct = st().tpsActive ? actualProgress(pl) : pl->getCurrentPercent();
    s.pausedPercent = pct;
    s.pausePending = true;
    log::info("AutoMacro: pauza na {:.1f}%", pct);

    if (!Mod::get()->getSettingValue<bool>("pause-start")) return;
    if (pct < 1.f) return;

    Loader::get()->queueInMainThread([pct]() {
        auto* pl = PlayLayer::get();
        if (!pl || !st().bot.isPlayback()) return;
        if (!pl->m_isPracticeMode) pl->togglePracticeMode(true);
        pl->markCheckpoint();
        notify(fmt::format("Pauza na {:.0f}% - restart zacznie od tego miejsca", pct));
    });
}

} // namespace am

// ------------------------------------------------------------------
// Hooki
// ------------------------------------------------------------------
class $modify(AMPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        am::onLevelEnter(level);
        return true;
    }

    // Jak w Eclipse Menu (BotPLHook::resetLevel): po resecie wylacz Click Between Frames i
    // cofnij makro na poczatek, gdy nie ma checkpointow. Gdy checkpoint istnieje, pozycja makra
    // jest ustawiana po klatce (seek), bo restart startuje ze srodka levelu.
    void resetLevel() {
        PlayLayer::resetLevel();
        auto& s = am::st();
        s.needSync = true;
        if (!s.bot.isPlayback()) return;

        am::cbfOff();
        if (m_checkpointArray->count() == 0) s.bot.restart();

        if (s.pausePending) {
            s.pausePending = false;
            float pct = am::st().tpsActive ? am::actualProgress(this) : this->getCurrentPercent();
            if (pct >= 1.f) am::notify(fmt::format("Makro startuje od {:.0f}%", pct));
        }
    }

    // --- TPS: poprawki PlayLayer z Eclipse (postUpdate / procenty / czas ukonczenia) ---
    void postUpdate(float dt) override {
        auto& s = am::st();
        if (!s.tpsActive) { PlayLayer::postUpdate(dt); return; }
        if (s.tpsHide) return; // dodatkowe kroki fizyki: pomijamy odswiezanie widoku
        PlayLayer::postUpdate(s.realDelta);
    }

    // GD liczy procenty ze stalym 240 TPS - przy innym TPS przeliczamy je z czasu.
    int fixProgressBegin() {
        auto saved = m_gameState.m_currentProgress;
        if (am::st().tpsActive && m_level->m_timestamp > 0) {
            float progress = am::actualProgress(this);
            m_gameState.m_currentProgress = m_level->m_timestamp * progress / 100.f;
        }
        return saved;
    }

    void updateProgressbar() {
        auto saved = fixProgressBegin();
        PlayLayer::updateProgressbar();
        m_gameState.m_currentProgress = saved;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) override {
        auto saved = fixProgressBegin();
        PlayLayer::destroyPlayer(player, object);
        m_gameState.m_currentProgress = saved;
    }

    void onQuit() {
        am::st().tpsActive = false;
        am::st().generation++;
        am::st().bot.unload();
        am::cbfRestore();
        PlayLayer::onQuit();
    }
};

class $modify(AMBaseGameLayer, GJBaseGameLayer) {
    // Blokada klikniec uzytkownika podczas odtwarzania makra. Wejscia makra przechodza przez ten
    // sam hook z ustawiona flaga "injecting" (inaczej wlasna blokada zjadalaby wejscia makra).
    void handleButton(bool down, int button, bool isPlayer1) {
        if (!am::st().injecting && am::blockingUserInput(this)) return;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    // --- TPS bypass (port z Eclipse Menu, TPSBypass.cpp) ---
    float customDelta(float dt, float tps, bool applyExtraDelta = true) {
        auto& s = am::st();
        auto spt = 1.f / tps;
        if (applyExtraDelta && m_resumeTimer > 0) {
            --m_resumeTimer;
            dt = 0.f;
        }
        auto totalDelta = dt + s.extraDelta;
        auto timestep = std::min(m_gameState.m_timeWarp, 1.f) * spt;
        auto steps = std::round(totalDelta / timestep);
        auto newDelta = steps * timestep;
        if (applyExtraDelta) s.extraDelta = totalDelta - newDelta;
        return static_cast<float>(newDelta);
    }

    float getModifiedDelta(float dt) {
        float tps = am::activeTps(this);
        if (tps <= 0.f) return GJBaseGameLayer::getModifiedDelta(dt);
        return customDelta(dt, tps);
    }

    void update(float dt) override {
        auto& s = am::st();
        float tps = am::activeTps(this);
        if (tps <= 0.f) {
            s.tpsActive = false;
            s.extraDelta = 0.0;
            GJBaseGameLayer::update(dt);
            return;
        }
        s.tpsActive = true;
        s.extraDelta += dt;
        s.realDelta = customDelta(dt, 240.f, false);

        double newDelta = 1.0 / tps;
        if (s.extraDelta >= newDelta) {
            size_t steps = size_t(s.extraDelta / newDelta);
            s.extraDelta -= steps * newDelta;

            auto start = am::nowMs();
            auto ms = dt * 1000;
            s.tpsHide = true;
            while (steps > 1) {
                GJBaseGameLayer::update(newDelta);
                // zbyt dlugi update - przerwij petle (jak w Eclipse)
                if (am::nowMs() - start > ms) break;
                --steps;
            }
            s.tpsHide = false;
            GJBaseGameLayer::update(newDelta * steps);
        }
    }

    // Jesli kompilator zglosi inna sygnature, dopasuj ja do bindingow Geode
    // (Eclipse dla GD 2.2074 uzywa wersji z jednym argumentem: processCommands(float dt)).
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (!am::macroActive(this) || m_levelEndAnimationStarted) return;

        auto& s = am::st();
        auto& bot = s.bot;
        int64_t offset = Mod::get()->getSettingValue<int64_t>("frame-offset");
        // Klatka makra liczona z czasu gry (m_levelTime rosnie o 1/TPS w kazdym kroku fizyki),
        // a nie z m_currentProgress - ten w GD 2.2081 ma inne jednostki (rosnie ~2 na krok).
        // Makro w klatce f (przy TPS makra = R) ma wejscie w chwili f / R.
        double rate = std::max(s.targetTps, 1.0);
        int64_t cur = int64_t(std::llround(m_gameState.m_levelTime * rate)) - offset;

        // Po resecie / restarcie z checkpointu: ustaw wskaznik makra na biezaca klatke i
        // zsynchronizuj stan wcisniecia przyciskow z tym, co makro mialo w tym miejscu.
        if (s.needSync) {
            s.needSync = false;
            s.lastBeat = -1;
            bot.seek(cur);
            for (int p2 = 0; p2 <= 1; ++p2) {
                if (p2 && !m_levelSettings->m_twoPlayerMode) break;
                PlayerObject* player = p2 ? m_player2 : m_player1;
                if (!player) continue;
                for (int btn = 1; btn <= 3; ++btn) {
                    bool want = bot.shouldBeHeld(p2 != 0, btn);
                    bool have = player->m_holdingButtons[btn];
                    if (want != have) {
                        s.injecting = true;
                        GJBaseGameLayer::handleButton(want, btn, p2 == 0);
                        s.injecting = false;
                    }
                }
            }
        }

        // Diagnostyka: raz na sekunde czasu gry wpis w logu ze stanem odtwarzacza
        int64_t sec = cur / std::max<int64_t>(int64_t(std::llround(rate)), 1);
        if (cur > 0 && sec != s.lastBeat) {
            s.lastBeat = sec;
            log::info("AutoMacro: stan: klatka={} czas={:.3f}s krok_licznika={} wejsc={} oczekujace={} odtworzono={} tps={}",
                      cur, m_gameState.m_levelTime, int64_t(m_gameState.m_currentProgress), bot.getInputCount(),
                      bot.hasPending(), s.applied, rate);
        }

        // Eclipse: while ((input = poll(m_currentProgress))) handleButton(...)
        while (auto input = bot.poll(cur)) {
            if (input->player2 && !m_levelSettings->m_twoPlayerMode) continue;
            s.injecting = true;
            GJBaseGameLayer::handleButton(input->down, input->button, !input->player2);
            s.injecting = false;

            if (s.applied < 40) {
                log::info("AutoMacro: wstrzykniete f={} {} p{} btn={} (klatka {})", input->frame,
                          input->down ? "DOWN" : "UP", input->player2 ? 2 : 1, int(input->button), cur);
            }
            s.applied++;
            if (!s.announced) {
                s.announced = true;
                am::notify("Makro odtwarza wejscia", NotificationIcon::Success);
            }
        }

        // Diagnostyka: makro wczytane, a po ~6 s gry nic nie odtworzono
        if (!s.stallWarned && s.applied == 0 && cur > 1440 && bot.getInputCount() > 0
            && bot.macro().inputs.front().frame < 1000) {
            s.stallWarned = true;
            am::notify("Makro wczytane, ale nic nie odtworzono - sprobuj zmienic 'Przesuniecie klatek' w ustawieniach",
                       NotificationIcon::Warning);
        }
    }
};

class $modify(AMPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        am::onPause();
    }
};
