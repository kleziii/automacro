#pragma once
// Zawiera kod pochodny od Eclipse Menu (modules/bot/bot.{hpp,cpp}, hacks/Bot/Bot.cpp),
// licencja: Eclipse Public License v2.0 (https://www.eclipse.org/legal/epl-2.0/).
// Zmodyfikowano: dostosowano do AutoMacro (automatyczne ladowanie makra, seek, brak nagrywania).
// Odtwarzacz makr - port modulu "bot" z Eclipse Menu (modules/bot/bot.{hpp,cpp}).
// Eclipse: ladowanie pliku, stan DISABLED/PLAYBACK, poll(frame), restart(), getPrevious().
// AutoMacro: te same operacje, ale makro trafia tu automatycznie (po pobraniu z API),
// a stan PLAYBACK wlacza sie sam - bez recznego wgrywania pliku i bez przelacznika w menu.

#include "MacroParse.hpp"

#include <algorithm>
#include <optional>

namespace am {

enum class BotState { DISABLED, PLAYBACK };

class Bot {
public:
    void setState(BotState s) { m_state = s; }
    [[nodiscard]] BotState getState() const { return m_state; }
    [[nodiscard]] bool isPlayback() const { return m_state == BotState::PLAYBACK && isLoaded(); }

    [[nodiscard]] bool isLoaded() const { return !m_macro.inputs.empty(); }
    [[nodiscard]] size_t getInputCount() const { return m_macro.inputs.size(); }
    [[nodiscard]] Macro const& macro() const { return m_macro; }

    // Wstawia pobrane makro (wejscia musza byc juz posortowane i przeliczone na 240 TPS).
    void load(Macro&& m) {
        m_macro = std::move(m);
        m_index = 0;
    }

    // Czysci makro i wylacza odtwarzanie (wyjscie z levelu).
    void unload() {
        m_macro = Macro{};
        m_index = 0;
        m_state = BotState::DISABLED;
    }

    // Eclipse: Bot::restart()
    void restart() { m_index = 0; }

    // Czy zostaly jeszcze wejscia do odtworzenia.
    [[nodiscard]] bool hasPending() const { return m_index < m_macro.inputs.size(); }

    // Eclipse: Bot::poll(frame) - nastepne wejscie, ktorego klatka <= biezaca klatka.
    // Osobne "biezace klatki" dla wcisniec i puszczen (strojenie przesuniec DOWN/UP).
    [[nodiscard]] std::optional<Input> poll(int64_t frameDown, int64_t frameUp) {
        if (m_index >= m_macro.inputs.size()) return std::nullopt;
        auto const& in = m_macro.inputs[m_index];
        if (int64_t(in.frame) <= (in.down ? frameDown : frameUp)) {
            ++m_index;
            return in;
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<Input> poll(int64_t frame) { return poll(frame, frame); }

    // Ustawia wskaznik na pierwsze wejscie o klatce >= frame (wznowienie w srodku levelu,
    // np. po restarcie z checkpointu). W Eclipse tego nie ma (tam tylko restart() do zera).
    void seek(int64_t frame) {
        uint64_t f = uint64_t(std::max<int64_t>(frame, 0));
        m_index = size_t(std::lower_bound(m_macro.inputs.begin(), m_macro.inputs.end(), f,
            [](Input const& in, uint64_t fr) { return in.frame < fr; }) - m_macro.inputs.begin());
    }

    // Eclipse: Bot::getPrevious() - ostatnie odtworzone wejscie danego przycisku/gracza.
    [[nodiscard]] std::optional<Input> getPrevious(bool player2, int button) const {
        for (size_t i = m_index; i-- > 0;) {
            auto const& in = m_macro.inputs[i];
            if (in.player2 == player2 && in.button == button) return in;
        }
        return std::nullopt;
    }

    // Czy przycisk powinien byc wcisniety po odtworzeniu wszystkich wejsc przed m_index.
    [[nodiscard]] bool shouldBeHeld(bool player2, int button) const {
        auto p = getPrevious(player2, button);
        return p && p->down;
    }

private:
    BotState m_state = BotState::DISABLED;
    Macro m_macro;
    size_t m_index = 0;
};

} // namespace am
