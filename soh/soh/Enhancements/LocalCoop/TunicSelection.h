// ShipCrew's local co-op tunic choices. No game assets or engine dependencies.
// This standalone state model is intentionally NOT wired into gameplay yet.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ShipCrew {

inline constexpr std::size_t kMaxLocalPlayers = 4;

enum class TunicColor : std::uint8_t { Green, Red, Blue, Purple, Yellow, Orange, White, Black, Pink, Cyan, Count };

struct Rgb {
    std::uint8_t r, g, b;
};

struct TunicPaletteEntry {
    TunicColor color;
    std::string_view name;
    Rgb previewRgb;
};

// The RGB values are initial UI/rendering targets, not final color grading.
inline constexpr std::array<TunicPaletteEntry, 10> kTunicPalette{ {
    { TunicColor::Green, "Green", { 35, 135, 43 } },
    { TunicColor::Red, "Red", { 177, 35, 42 } },
    { TunicColor::Blue, "Blue", { 37, 82, 186 } },
    { TunicColor::Purple, "Purple", { 111, 62, 157 } },
    { TunicColor::Yellow, "Yellow", { 218, 185, 39 } },
    { TunicColor::Orange, "Orange", { 210, 101, 28 } },
    { TunicColor::White, "White", { 232, 233, 224 } },
    { TunicColor::Black, "Black", { 36, 37, 46 } },
    { TunicColor::Pink, "Pink", { 217, 105, 158 } },
    { TunicColor::Cyan, "Cyan", { 41, 173, 185 } },
} };

inline constexpr std::array<TunicColor, kMaxLocalPlayers> kDefaultTunicChoices{ TunicColor::Green, TunicColor::Red,
                                                                                TunicColor::Blue, TunicColor::Purple };

[[nodiscard]] constexpr bool IsValidTunicColor(TunicColor color) {
    return static_cast<std::size_t>(color) < kTunicPalette.size();
}

class TunicSelection {
  public:
    using Preferences = std::array<std::uint8_t, kMaxLocalPlayers>;

    explicit TunicSelection(Preferences preferences = { 0, 1, 2, 3 }) : preferences_(preferences) {
        // Invalid/corrupted preferences must never index the palette.
        for (std::size_t i = 0; i < kMaxLocalPlayers; ++i) {
            if (preferences_[i] >= kTunicPalette.size()) {
                preferences_[i] = static_cast<std::uint8_t>(kDefaultTunicChoices[i]);
            }
        }
    }

    // Only joined slots reserve colors; the default choices aren't reservations.
    [[nodiscard]] bool Join(std::size_t slot) {
        if (slot >= kMaxLocalPlayers) {
            return false;
        }
        if (active_[slot].has_value()) {
            return true;
        }

        TunicColor preferred = static_cast<TunicColor>(preferences_[slot]);
        if (!TakenByAnother(slot, preferred)) {
            active_[slot] = preferred;
            return true;
        }

        // Recover deterministically from duplicate saved preferences.
        for (const auto& entry : kTunicPalette) {
            if (!TakenByAnother(slot, entry.color)) {
                active_[slot] = entry.color;
                preferences_[slot] = static_cast<std::uint8_t>(entry.color);
                return true;
            }
        }
        return false;
    }

    // Changing one player's choice never silently changes another's.
    [[nodiscard]] bool Select(std::size_t slot, TunicColor color) {
        if (slot >= kMaxLocalPlayers || !active_[slot].has_value() || !IsValidTunicColor(color) ||
            TakenByAnother(slot, color)) {
            return false;
        }
        active_[slot] = color;
        preferences_[slot] = static_cast<std::uint8_t>(color);
        return true;
    }

    // Call only on confirmed leave. A temporary controller disconnect should
    // retain the slot reservation until the outer session manager releases it.
    void Leave(std::size_t slot) {
        if (slot < kMaxLocalPlayers) {
            active_[slot].reset();
        }
    }

    [[nodiscard]] std::optional<TunicColor> ColorFor(std::size_t slot) const {
        return slot < kMaxLocalPlayers ? active_[slot] : std::nullopt;
    }

    [[nodiscard]] bool AvailableTo(std::size_t slot, TunicColor color) const {
        return slot < kMaxLocalPlayers && IsValidTunicColor(color) && !TakenByAnother(slot, color);
    }

    [[nodiscard]] Preferences SavedPreferences() const {
        return preferences_;
    }

  private:
    [[nodiscard]] bool TakenByAnother(std::size_t slot, TunicColor color) const {
        for (std::size_t other = 0; other < kMaxLocalPlayers; ++other) {
            if (other != slot && active_[other] == color) {
                return true;
            }
        }
        return false;
    }

    Preferences preferences_;
    std::array<std::optional<TunicColor>, kMaxLocalPlayers> active_{};
};

} // namespace ShipCrew
