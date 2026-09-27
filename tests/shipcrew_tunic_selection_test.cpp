// Standalone tests: g++ -std=c++20 -Wall -Wextra -Werror -I. \\
//     tests/shipcrew_tunic_selection_test.cpp -o /tmp/shipcrew_tunic_test && /tmp/shipcrew_tunic_test
#include "soh/soh/Enhancements/LocalCoop/TunicSelection.h"

#include <cassert>
#include <cstdint>
#include <iostream>

using ShipCrew::TunicColor;
using ShipCrew::TunicSelection;

int main() {
    static_assert(ShipCrew::kTunicPalette.size() == 10);
    static_assert(ShipCrew::kMaxLocalPlayers == 4);

    TunicSelection selection;
    for (std::size_t slot = 0; slot < 4; ++slot) {
        assert(selection.Join(slot));
        assert(selection.ColorFor(slot) == ShipCrew::kDefaultTunicChoices[slot]);
    }
    assert(!selection.Join(4));
    assert(!selection.AvailableTo(3, TunicColor::Green));
    assert(!selection.Select(1, TunicColor::Green));   // duplicate rejected
    assert(selection.ColorFor(1) == TunicColor::Red);  // earlier choice unaffected
    assert(selection.Select(1, TunicColor::Cyan));
    assert(selection.ColorFor(1) == TunicColor::Cyan);
    assert(!selection.Select(1, static_cast<TunicColor>(250)));
    assert(!selection.Select(4, TunicColor::White));

    selection.Leave(1);
    assert(!selection.ColorFor(1).has_value());
    assert(selection.Join(1));  // slot's chosen color persists in preferences
    assert(selection.ColorFor(1) == TunicColor::Cyan);
    assert(selection.SavedPreferences()[1] == static_cast<std::uint8_t>(TunicColor::Cyan));

    // Corrupt & duplicate preferences are resolved without assigning same color twice.
    TunicSelection recovered({ 250, 0, 0, 0 });
    assert(recovered.Join(0));  // invalid becomes Green
    assert(recovered.Join(1));  // saved Green taken -> Red
    assert(recovered.Join(2));  // saved Green taken -> Blue
    assert(recovered.Join(3));  // saved Green taken -> Purple
    assert(recovered.ColorFor(0) == TunicColor::Green);
    assert(recovered.ColorFor(1) == TunicColor::Red);
    assert(recovered.ColorFor(2) == TunicColor::Blue);
    assert(recovered.ColorFor(3) == TunicColor::Purple);
    recovered.Leave(0);
    assert(recovered.AvailableTo(1, TunicColor::Green));
    assert(recovered.Select(1, TunicColor::Green));

    std::cout << "ShipCrew tunic selection tests passed (10 colors, unique choices, recovery).\\n";
}
