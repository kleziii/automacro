// AutoMacro - wykrywa level, pobiera makro (Hyperbolus / wlasne zrodla) i odtwarza je.
// Geode SDK 5.x, Geometry Dash 2.2081

#include "MacroParse.hpp"
#include "Playback.hpp"

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
constexpr char const* kCbfId = "syzzi.click_between_frames";

// ------------------------------------------------------------------
// Stan
// ------------------------------------------------------------------
struct State {
    std::optional<Macro> macro;
    Replayer rp;                  // kursor odtwarzania + stan przytrzymania wedlug makra
    bool needSync = true;
    int generation = 0;
    bool injecting = false;       // true, gdy to MY wstrzykujemy przycisk (nie uzytkownik)
    size_t applied = 0;           // ile wejsc odtworzono w tym levelu
    bool announced = false;
    bool stallWarned = false;
    bool pausePending = false;    // uzytkownik spauzowal, czekamy na restart
    float pausedPercent = 0.f;
    int injectLogs = 0;           // ile pierwszych wstrzyknien zalogowano (diagnostyka)
    int repairs = 0;              // ile razy naprawiono przerwane przytrzymanie
    std::optional<bool> cbfPrev;  // poprzednia wartosc "soft-toggle" w Click Between Frames
    std::set<uint32_t> missing; // levele, dla ktorych w tej sesji nic nie znaleziono
    std::deque<std::chrono::steady_clock::time_point> requests;
};

static State& st() {
    static State s;
    return s;
}

static void notify(std::string const& msg, NotificationIcon icon = NotificationIcon::Info) {
    if (!Mod::get()->getSettingValue<bool>("notifications")) return;
    Notification::create(msg, icon)->show();
}

// ------------------------------------------------------------------
// Click Between Frames: przy wlaczonym CBF wejscia wstrzykiwane przez mod potrafia byc przejmowane lub
// opozniane przez jego wlasna kolejke wejsc (efekt: klik jest, przytrzymania nie ma). zBot robi to samo -
// na czas odtwarzania wlacza "soft-toggle" w CBF (patrz zBot/src/clickbetweenframescompat.cpp).
// Poprzednia wartosc przywracamy po wyjsciu z levelu.
// ------------------------------------------------------------------
static void restoreCbf() {
    auto& s = st();
    if (!s.cbfPrev) return;
    if (auto* cbf = Loader::get()->getLoadedMod(kCbfId)) {
        if (cbf->hasSetting("soft-toggle")) cbf->setSettingValue<bool>("soft-toggle", *s.cbfPrev);
    }
    s.cbfPrev.reset();
}

static void applyCbfCompat() {
    auto& s = st();
    bool want = s.macro.has_value() && Mod::get()->getSettingValue<bool>("autoplay") &&
                Mod::get()->getSettingValue<bool>("cbf-compat");
    if (!want) { restoreCbf(); return; }

    auto* cbf = Loader::get()->getLoadedMod(kCbfId);
    if (!cbf || !cbf->hasSetting("soft-toggle")) return;
    if (!s.cbfPrev) s.cbfPrev = cbf->getSettingValue<bool>("soft-toggle");
    if (!cbf->getSettingValue<bool>("soft-toggle")) {
        cbf->setSettingValue<bool>("soft-toggle", true);
        log::info("AutoMacro: wlaczono soft-toggle w Click Between Frames na czas odtwarzania makra");
    }
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
    double scale = 240.0 / m.framerate;
    bool rescaled = std::abs(scale - 1.0) > 1e-6;
    if (rescaled) {
        for (auto& in : m.inputs) in.frame = uint64_t(std::llround(double(in.frame) * scale));
    }
    sortInputs(m);

    if (fromNetwork) saveCache(id, data);

    auto& s = st();
    size_t count = m.inputs.size();
    double fps = m.framerate;
    log::info("AutoMacro: wczytano makro ({}), {} wejsc, {} wcisniec, {} TPS, pierwsza klatka {}",
              origin, count, presses, fps, m.inputs.front().frame);
    if (m.skippedEvents > 0) {
        log::info("AutoMacro: pominieto {} zdarzen niebedacych wejsciami (korekty fizyki)", m.skippedEvents);
    }
    s.macro = std::move(m);
    s.rp.reset();
    s.needSync = true;
    s.applied = 0;
    s.announced = false;
    s.stallWarned = false;
    s.injectLogs = 0;
    s.repairs = 0;
    applyCbfCompat();

    if (rescaled) {
        notify(fmt::format("Makro wczytane ({} wejsc, {} wcisniec), ale nagrane w {:.0f} TPS - moze sie rozjezdzac", count, presses, fps),
               NotificationIcon::Warning);
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
    s.macro.reset();
    s.rp.reset();
    s.injectLogs = 0;
    s.repairs = 0;
    restoreCbf();
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
// Odtwarzanie
// ------------------------------------------------------------------
static bool macroActive(GJBaseGameLayer* layer) {
    if (!typeinfo_cast<PlayLayer*>(layer)) return false;
    if (!st().macro) return false;
    return Mod::get()->getSettingValue<bool>("autoplay");
}

// Czy mamy blokowac klikniecia uzytkownika? Tak, dopoki makro ma jeszcze wejscia do odtworzenia.
static bool blockingUserInput(GJBaseGameLayer* layer) {
    if (!macroActive(layer)) return false;
    if (!Mod::get()->getSettingValue<bool>("block-input")) return false;
    auto& s = st();
    return !s.rp.finished(s.macro->inputs);
}

static PlayerObject* playerOf(GJBaseGameLayer* g, bool player2) {
    return player2 ? g->m_player2 : g->m_player1;
}

// Wstrzykuje jedno wejscie (wcisniecie ALBO puszczenie). Zwraca true, jesli zostalo wykonane.
// Przytrzymanie = wcisniecie w klatce f i puszczenie dopiero w klatce g: gra trzyma przycisk (m_holdingButtons)
// az do wejscia z down=false, wiec kazde z nich musi trafic do GJBaseGameLayer::handleButton (jak w zBot).
static bool inject(GJBaseGameLayer* g, Input const& in) {
    if (in.player2 && !g->m_levelSettings->m_twoPlayerMode) return false;
    auto& s = st();
    s.injecting = true;
    g->handleButton(in.down, in.button, !in.player2);
    s.injecting = false;

    // Diagnostyka: pierwsze wstrzykniecia z odczytem tego, co gra faktycznie zrobila z przyciskiem.
    if (s.injectLogs < 12 && in.button == 1) {
        s.injectLogs++;
        auto* pl = playerOf(g, in.player2);
        log::info("AutoMacro: wejscie klatka={} down={} p2={} -> gra trzyma skok={} (licznik klatek {})",
                  in.frame, in.down, in.player2, pl ? pl->buttonDown(PlayerButton::Jump) : false,
                  g->m_gameState.m_currentProgress);
    }
    return true;
}

// Pilnowanie przytrzymania: jesli makro mowi "skok wcisniety", a gra go nie trzyma (cos spoza makra puscilo
// przycisk, np. wznowienie po pauzie albo inny mod) - wciskamy ponownie. Dziala tylko, gdy blokujemy wejscia
// gracza (inaczej moglibysmy walczyc z jego wlasnymi klikami) i tylko dla skoku.
static void keepHolds(GJBaseGameLayer* g) {
    if (!Mod::get()->getSettingValue<bool>("hold-watchdog")) return;
    if (!blockingUserInput(g)) return;
    auto& s = st();

    int fixed = s.rp.repairHolds(
        [g](int key) {
            if (keyButton(key) != 1) return true; // tylko skok
            auto* pl = playerOf(g, keyIsP2(key));
            if (!pl || pl->m_isDead) return true; // nie da sie sprawdzic / gracz nie zyje -> nie ruszamy
            return pl->buttonDown(PlayerButton::Jump);
        },
        [g, &s](int key) {
            bool p2 = keyIsP2(key);
            Input fix;
            fix.button = 1;
            fix.player2 = p2;
            fix.down = true;
            inject(g, fix);

            // Jesli handleButton zostal polkniety (np. przez inny mod) - wciskamy przycisk bezposrednio.
            auto* pl = playerOf(g, p2);
            if (pl && !pl->buttonDown(PlayerButton::Jump)) {
                pl->pushButton(PlayerButton::Jump);
                bool mirrorP2 = !p2 && !g->m_levelSettings->m_twoPlayerMode && g->m_gameState.m_isDualMode;
                if (mirrorP2 && g->m_player2) g->m_player2->pushButton(PlayerButton::Jump);
            }
            if (s.repairs < 20) {
                log::warn("AutoMacro: przytrzymanie zostalo przerwane przez cos spoza makra - wcisnieto ponownie (klatka {})",
                          g->m_gameState.m_currentProgress);
            }
            s.repairs++;
        });
    (void)fixed;
}

// Wolane PO oryginalnym processCommands (tak samo jak w zBot): wejscie z klatka f jest
// wstrzykiwane, gdy licznik klatek (m_currentProgress) przekroczy f.
static void playback(GJBaseGameLayer* g) {
    if (!macroActive(g) || g->m_levelEndAnimationStarted) return;

    auto& s = st();
    auto const& inputs = s.macro->inputs;
    int64_t offset = Mod::get()->getSettingValue<int64_t>("frame-offset");
    int64_t cur = int64_t(g->m_gameState.m_currentProgress) - offset;

    auto apply = [g](Input const& in) { return inject(g, in); };

    // Wznowienie w srodku levelu (restart z checkpointu po pauzie): ustaw kursor i odtworz stan
    // wcisniecia przyciskow, jaki makro mialo w tym miejscu.
    if (s.needSync) {
        s.needSync = false;
        s.rp.sync(inputs, cur, apply);
    }

    size_t n = s.rp.step(inputs, cur, apply);
    if (n > 0) {
        s.applied += n;
        if (!s.announced) {
            s.announced = true;
            log::info("AutoMacro: pierwsze wejscie odtworzone (licznik {})", cur);
            notify("Makro odtwarza wejscia", NotificationIcon::Success);
        }
    }

    keepHolds(g);

    // Diagnostyka: makro wczytane, a po ~6 s gry nic nie odtworzono
    if (!s.stallWarned && s.applied == 0 && cur > 1440 && !inputs.empty() && inputs.front().frame < 1000) {
        s.stallWarned = true;
        notify("Makro wczytane, ale nic nie odtworzono - sprobuj zmienic 'Przesuniecie klatek' w ustawieniach",
               NotificationIcon::Warning);
    }
}

// ------------------------------------------------------------------
// Pauza: zapamietaj % i zrob checkpoint, zeby restart zaczynal od tego miejsca
// ------------------------------------------------------------------
static void onPause() {
    auto* pl = PlayLayer::get();
    auto& s = st();
    if (!pl || !s.macro) return;

    float pct = pl->getCurrentPercent();
    s.pausedPercent = pct;
    s.pausePending = true;
    log::info("AutoMacro: pauza na {:.1f}%", pct);

    if (!Mod::get()->getSettingValue<bool>("pause-start")) return;
    if (pct < 1.f) return;

    Loader::get()->queueInMainThread([pct]() {
        auto* pl = PlayLayer::get();
        if (!pl || !st().macro) return;
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

    void resetLevel() {
        PlayLayer::resetLevel();
        auto& s = am::st();
        s.needSync = true;
        am::applyCbfCompat();
        if (s.pausePending && s.macro) {
            s.pausePending = false;
            float pct = this->getCurrentPercent();
            if (pct >= 1.f) am::notify(fmt::format("Makro startuje od {:.0f}%", pct));
        }
    }

    void onQuit() {
        am::st().generation++;
        am::st().macro.reset();
        am::st().rp.reset();
        am::restoreCbf();
        PlayLayer::onQuit();
    }

    void onExit() {
        am::restoreCbf();
        PlayLayer::onExit();
    }
};

class $modify(AMBaseGameLayer, GJBaseGameLayer) {
    // Blokada klikniec uzytkownika podczas odtwarzania makra.
    void handleButton(bool down, int button, bool isPlayer1) {
        if (!am::st().injecting && am::blockingUserInput(this)) return;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    // Jesli kompilator zglosi inna sygnature, dopasuj ja do bindingow Geode.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        am::playback(this);
    }
};

class $modify(AMPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        am::onPause();
    }
};
