#include "core/mining.hpp"

#include <cassert>
#include <string>

int main() {
    unique_rx::core::MiningState state;
    unique_rx::core::hash_worker(state, 0, 0, 0);

    assert(state.stop.load());
    {
        std::lock_guard lock(state.error_mutex);
        assert(state.worker_error == "invalid CPU worker index or worker count");
    }

    unique_rx::core::MiningState invalid_index_state;
    unique_rx::core::hash_worker(invalid_index_state, 2, 2, 0);
    assert(invalid_index_state.stop.load());
    {
        std::lock_guard lock(invalid_index_state.error_mutex);
        assert(invalid_index_state.worker_error == "invalid CPU worker index or worker count");
    }

    return 0;
}
