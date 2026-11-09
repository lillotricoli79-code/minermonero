/*
Copyright (c) 2018-2019, tevador <tevador@gmail.com>

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
	* Redistributions of source code must retain the above copyright
	  notice, this list of conditions and the following disclaimer.
	* Redistributions in binary form must reproduce the above copyright
	  notice, this list of conditions and the following disclaimer in the
	  documentation and/or other materials provided with the distribution.
	* Neither the name of the copyright holder nor the
	  names of its contributors may be used to endorse or promote products
	  derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <new>
#include <mutex>
#include <vector>
#include "allocator.hpp"
#include "intrin_portable.h"
#include "virtual_memory.h"
#include "common.hpp"

namespace {
    constexpr size_t scratchpadSize = 2UL << 20;
    thread_local int allocationNode = -1;
    struct CachedPage { void* ptr; bool huge; int node; };
    struct ScratchpadPool {
        std::mutex mutex;
        std::vector<CachedPage> pages;
        size_t capacity = 0;

        ~ScratchpadPool() {
            for (const auto& page : pages)
                freePagedMemory(page.ptr, scratchpadSize);
        }

        void* take(size_t count, bool huge) {
            if (count != scratchpadSize) return nullptr;
            std::lock_guard<std::mutex> guard(mutex);
            for (auto it = pages.begin(); it != pages.end(); ++it) {
                if (it->huge == huge && it->node == allocationNode) {
                    void* ptr = it->ptr;
                    pages.erase(it);
                    return ptr;
                }
            }
            return nullptr;
        }

        bool put(void* ptr, size_t count, bool huge) {
            if (!ptr || count != scratchpadSize) return false;
            std::lock_guard<std::mutex> guard(mutex);
            if (pages.size() >= capacity) return false;
            try { pages.push_back({ptr, huge, allocationNode}); }
            catch (...) { return false; }
            return true;
        }

        void set_capacity(size_t next_capacity) {
            std::lock_guard<std::mutex> guard(mutex);
            for (const auto& page : pages)
                freePagedMemory(page.ptr, scratchpadSize);
            pages.clear();
            capacity = next_capacity;
        }
    } scratchpad_pool;
}

extern "C" void rxm_set_memory_pool(size_t capacity) {
    scratchpad_pool.set_capacity(capacity);
}

extern "C" void rxm_set_allocator_node(int node) {
    allocationNode = node;
}

namespace randomx {

	template<size_t alignment>
	void* AlignedAllocator<alignment>::allocMemory(size_t count) {
		/* RxMiner MIT adaptation from commit 5fe52dde2650c623fd0472804152a94711889dce:
         * cache, dataset and 2 MiB worker scratchpads use
         * page-backed mappings; scratchpads can be reused after VM teardown.
         * Page alignment is sufficient for every RandomX alignment requirement. */
		void *mem = scratchpad_pool.take(count, false);
        if (!mem) mem = allocMemoryPages(count);
		if (mem == nullptr)
			throw std::bad_alloc();
		return mem;
	}

	template<size_t alignment>
	void AlignedAllocator<alignment>::freeMemory(void* ptr, size_t count) {
		if (!scratchpad_pool.put(ptr, count, false))
            freePagedMemory(ptr, count);
	}

	template struct AlignedAllocator<CacheLineSize>;

	void* LargePageAllocator::allocMemory(size_t count) {
		void *mem = scratchpad_pool.take(count, true);
        if (!mem) mem = allocLargePagesMemory(count);
		if (mem == nullptr)
			throw std::bad_alloc();
		return mem;
	}

	void LargePageAllocator::freeMemory(void* ptr, size_t count) {
        if (!scratchpad_pool.put(ptr, count, true))
            freePagedMemory(ptr, alignSize(count, 2UL << 20));
	};

}
