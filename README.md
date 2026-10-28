# AutoMacro (Geode 5.x, GD 2.2081)

Wykrywa level, szuka makra, pobiera je i od razu odtwarza.

## Przeplyw
1. `PlayLayer::init` -> ID levelu.
2. Lokalnie: `<save dir moda>/macros/<ID>.macro` (lub .gdr2/.gdr/.json) - pobrane makra sa tu cache'owane.
3. Hyperbolus: `GET https://hyperbolus.net/api/macros?level_id=<ID>`
   - bierze pierwszy plik z kazdego wpisu, odrzuca nie-https, puste i > 16 MB,
   - sortuje: zatwierdzone (approved_at) > format (gdr2, gdr, mhr, json) > liczba pobran,
   - probuje pobrac max 5 najlepszych, az ktores sie sparsuje.
4. Opcjonalnie wlasne zrodla (szablon URL z `{id}` albo indeks JSON).
5. Odtwarzanie (port z Eclipse Menu, `src/Bot.hpp`): pobrane makro trafia do `Bot::load()` i stan
   PLAYBACK wlacza sie samo. W hooku `processCommands` PO oryginale: `while (poll(m_currentProgress)) handleButton(...)`
   (wejscie z klatka f odtwarzane, gdy f <= licznik). Wejscia ida wprost przez `GJBaseGameLayer::handleButton`.
   Po resecie/checkpoincie `seek()` ustawia makro na biezaca klatke i synchronizuje stan przyciskow.
   Dostrajanie: ustawienie "Przesuniecie klatek".
6. Blokada klikniec gracza: hook `handleButton` ignoruje klikniecia, dopoki makro ma wejscia do odtworzenia.
7. Pauza: hook `PauseLayer::customSetup` zapamietuje % i (opcja) wlacza practice + `markCheckpoint()`;
   restart startuje z checkpointu, a makro synchronizuje sie po klatce i odtwarza stan wcisniecia przyciskow.

## Formaty
- GDR2 - przez libGDR (CPM, ten sam commit co w Click Guide Visualizer)
- GDR1 (msgpack), JSON, MHR (JSON) - wlasny parser (`src/Tree.hpp`, `src/MacroParse.hpp`),
  format rozpoznawany po zawartosci pliku.
- zBot: stary binarny .zbf (zbot, zbot_frame) i nowy .gdr z zBot v3 (GDR1/msgpack).
- echo / tasbot / ybot / rush itd. NIE sa obslugiwane (takie wpisy sa pomijane).

## Dobre maniery wobec API
Limit po stronie klienta: 30 zapytan/min (Hyperbolus dopuszcza 60). Wynik jest cache'owany na dysku,
a levele bez makra nie sa odpytywane ponownie w tej samej sesji. User-Agent identyfikuje mod.

## Budowanie
Geode SDK 5.8.2+ i Geode CLI, ustaw `GEODE_SDK`, potem `geode build`.

## Znane ryzyka
- Sygnatura `processCommands` bywa rozna w bindingach (1 lub 3 argumenty) - dopasuj w `src/main.cpp`.
- Makra z samymi inputami (bez korekt fizyki) moga sie rozjechac; makra w TPS != 240 sa przeskalowane (mod ostrzega).
- API Hyperbolusa nie jest oficjalnie udokumentowane - schemat odczytany z kodu Click Guide Visualizer.

## Budowanie na GitHubie (bez instalowania niczego lokalnie)
Workflow `.github/workflows/build.yml` buduje wersje na Windows (Geode 5.8.2). Wynik: artefakt z plikiem `.geode`.

## Diagnostyka (log Geode)
Linie `AutoMacro:` w logu pokazuja format makra, pierwsze 12 zdarzen (DOWN/UP) i pierwsze 40 wstrzyknietych wejsc.
Log: wlacz "Show Platform Console" w ustawieniach Geode albo otworz najnowszy plik w `geode/logs`.

## Click Between Frames
Jesli zainstalowany jest `syzzi.click_between_frames`, mod wylacza go (soft-toggle) na czas odtwarzania makra - tak jak Eclipse Menu - i przywraca przy wyjsciu z levelu.
