#include <cassert>
#include <iostream>
#include <vector>

#include "book/order_pool.hpp"

using namespace book;

int main() {
    OrderPool pool(4);
    assert(pool.capacity() == 4);

    int32_t a = pool.alloc();
    int32_t b = pool.alloc();
    int32_t c = pool.alloc();
    int32_t d = pool.alloc();
    assert(a != kNullIndex && b != kNullIndex && c != kNullIndex && d != kNullIndex);
    assert(a != b && b != c && c != d);
    std::cout << "[PASS] allocated 4 distinct slots from capacity-4 pool\n";

    int32_t e = pool.alloc();
    assert(e == kNullIndex);
    std::cout << "[PASS] pool correctly reports exhaustion on 5th alloc\n";

    pool[a].order_ref_number = 1001;
    pool[a].price_raw = 1500000;
    pool[a].shares = 500;
    pool[a].side = 'B';
    assert(pool[a].order_ref_number == 1001);
    assert(pool[a].in_use == true);
    std::cout << "[PASS] wrote and read fields on allocated slot\n";

    pool.free(b);
    int32_t reused = pool.alloc();
    assert(reused == b);
    assert(pool[reused].in_use == true);
    assert(pool[reused].order_ref_number == 0); // freed slot resets to defaults
    std::cout << "[PASS] freed slot correctly reused and reset\n";

    pool.free(a);
    pool.free(c);
    pool.free(d);
    pool.free(reused);

    std::vector<int32_t> reallocated;
    for (int i = 0; i < 4; ++i) {
        int32_t idx = pool.alloc();
        assert(idx != kNullIndex);
        reallocated.push_back(idx);
    }
    assert(pool.alloc() == kNullIndex);
    std::cout << "[PASS] full free/realloc cycle correct, pool capacity stable\n";

    std::cout << "All OrderPool tests passed.\n";
    return 0;
}
