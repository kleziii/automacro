#pragma once
// Czysta logika odtwarzania makra (bez zaleznosci od Geode) - dzieki temu da sie ja testowac lokalnie
// (patrz tests/). Odpowiada za:
//   * wstrzykiwanie wejsc w odpowiednich klatkach (ta sama konwencja co zBot: frame < licznik klatek),
//   * sledzenie stanu "przycisk wcisniety / puszczony" wedlug makra (czyli przytrzymania),
//   * odtworzenie stanu przytrzymania po wznowieniu w srodku makra (restart z checkpointu),
//   * naprawe przytrzymania, ktore zostalo przerwane przez cos spoza makra.

#include "MacroParse.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

namespace am {

// Klucz = (gracz2 ? 4 : 0) + przycisk (1 = skok, 2 = lewo, 3 = prawo).
inline int keyOf(bool player2, int button) { return (player2 ? 4 : 0) + button; }
inline int keyOf(Input const& in) { return keyOf(in.player2, in.button); }
inline bool keyIsP2(int key) { return key >= 4; }
inline int keyButton(int key) { return key >= 4 ? key - 4 : key; }

struct Replayer {
    size_t idx = 0;           // nastepne wejscie do odtworzenia
    std::map<int, bool> held; // stan przyciskow wedlug makra: true = ma byc przytrzymany

    void reset() {
        idx = 0;
        held.clear();
    }

    bool finished(std::vector<Input> const& inputs) const { return idx >= inputs.size(); }

    // Ustawia kursor na klatke `cur` i odtwarza stan wcisniecia, jaki makro mialo w tym miejscu
    // (np. po restarcie z checkpointu w trakcie przytrzymania).
    // `apply(Input)` zwraca true, jesli wejscie zostalo faktycznie wykonane.
    template <class Apply>
    void sync(std::vector<Input> const& inputs, int64_t cur, Apply&& apply) {
        held.clear();
        uint64_t from = uint64_t(std::max<int64_t>(cur, 0));
        idx = size_t(std::lower_bound(inputs.begin(), inputs.end(), from,
                                      [](Input const& in, uint64_t f) { return in.frame < f; }) -
                     inputs.begin());

        std::map<int, Input const*> last;
        for (size_t k = 0; k < idx; ++k) last[keyOf(inputs[k])] = &inputs[k];
        for (auto& [key, in] : last) {
            if (in->down && apply(*in)) held[key] = true;
        }
    }

    // Wykonuje wszystkie wejscia z klatka < cur. Zwraca liczbe wykonanych.
    template <class Apply>
    size_t step(std::vector<Input> const& inputs, int64_t cur, Apply&& apply) {
        size_t n = 0;
        while (idx < inputs.size() && int64_t(inputs[idx].frame) < cur) {
            Input const& in = inputs[idx++];
            if (apply(in)) {
                held[keyOf(in)] = in.down;
                ++n;
            }
        }
        return n;
    }

    // Pilnowanie przytrzymania: dla kazdego przycisku, ktory wedlug makra ma byc wcisniety,
    // sprawdza czy gra faktycznie go trzyma. Jesli nie - wola fix(key).
    // `isDown(key)` ma zwracac true tez wtedy, gdy stanu nie da sie sprawdzic (nie naprawiamy "na slepo").
    template <class IsDown, class Fix>
    int repairHolds(IsDown&& isDown, Fix&& fix) {
        int n = 0;
        for (auto& [key, want] : held) {
            if (!want) continue;
            if (isDown(key)) continue;
            fix(key);
            ++n;
        }
        return n;
    }
};

} // namespace am
