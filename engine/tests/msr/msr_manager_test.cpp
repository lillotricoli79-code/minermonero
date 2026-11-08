#include "msr/msr.hpp"

#include <cassert>

int main() {
    unique_rx::MsrManager manager;
    assert(!manager.active());

    manager.restore();
    assert(!manager.active());

    manager.restore();
    assert(!manager.active());

    return 0;
}
