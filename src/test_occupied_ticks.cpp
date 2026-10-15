#include <cassert>
#include <iostream>

#include "book/occupied_ticks.hpp"

using namespace book;

int main() {
    OccupiedTicks ticks(300);
    ticks.set(0);
    ticks.set(5);
    ticks.set(63);
    ticks.set(64);
    ticks.set(200);

    assert(ticks.next_below(0) == -1);
    assert(ticks.next_below(5) == 0);
    assert(ticks.next_below(64) == 63);
    assert(ticks.next_below(201) == 200);
    assert(ticks.next_above(0) == 5);
    assert(ticks.next_above(5) == 63);
    assert(ticks.next_above(63) == 64);
    assert(ticks.next_above(200) == -1);
    std::cout << "[PASS] next occupied tick crosses word boundaries\n";

    ticks.clear(63);
    assert(ticks.next_above(5) == 64);
    assert(ticks.next_below(64) == 5);
    std::cout << "[PASS] clearing a tick removes it from both directions\n";

    std::cout << "All OccupiedTicks tests passed.\n";
    return 0;
}
