// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "common/heap_tracker.h"
#include "common/host_memory.h"
#include "common/literals.h"
#include "common/scope_exit.h"

using Common::HostMemory;
using namespace Common::Literals;

static constexpr size_t VIRTUAL_SIZE = 1ULL << 39;
static constexpr size_t BACKING_SIZE = 4_GiB;
static constexpr auto PERMS = Common::MemoryPermission::ReadWrite;
static constexpr auto HEAP = false;

TEST_CASE("HostMemory: Initialize and deinitialize", "[common]") {
    {
        HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
        REQUIRE(mem.BackingBasePointer() != nullptr);
    }
    {
        HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
        REQUIRE(mem.BackingBasePointer() != nullptr);
    }
}

TEST_CASE("HostMemory: Simple map", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x8000, 0x1000, PERMS, HEAP);

    volatile u8* const data = mem.VirtualBasePointer() + 0x5000;
    data[0] = 50;
    REQUIRE(data[0] == 50);
}

TEST_CASE("HostMemory: Simple mirror map", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);
    mem.Map(0x8000, 0x4000, 0x1000, PERMS, HEAP);

    volatile u8* const mirror_a = mem.VirtualBasePointer() + 0x5000;
    volatile u8* const mirror_b = mem.VirtualBasePointer() + 0x8000;
    mirror_b[0] = 76;
    REQUIRE(mirror_a[0x1000] == 76);
}

TEST_CASE("HostMemory: Simple unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);

    volatile u8* const data = mem.VirtualBasePointer() + 0x5000;
    data[75] = 50;
    REQUIRE(data[75] == 50);

    mem.Unmap(0x5000, 0x2000, HEAP);
}

TEST_CASE("HostMemory: Simple unmap and remap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);

    volatile u8* const data = mem.VirtualBasePointer() + 0x5000;
    data[0] = 50;
    REQUIRE(data[0] == 50);

    mem.Unmap(0x5000, 0x2000, HEAP);

    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);
    REQUIRE(data[0] == 50);

    mem.Map(0x7000, 0x2000, 0x5000, PERMS, HEAP);
    REQUIRE(data[0x3000] == 50);
}

TEST_CASE("HostMemory: Nieche allocation", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x20000, PERMS, HEAP);
    mem.Unmap(0x0000, 0x4000, HEAP);
    mem.Map(0x1000, 0, 0x2000, PERMS, HEAP);
    mem.Map(0x3000, 0, 0x1000, PERMS, HEAP);
    mem.Map(0, 0, 0x1000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Full unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x8000, 0x4000, HEAP);
    mem.Map(0x6000, 0, 0x16000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Right out of bounds unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x2000, 0x4000, HEAP);
    mem.Map(0x2000, 0x80000, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Left out of bounds unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);

    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x6000, 0x4000, HEAP);
    mem.Map(0x8000, 0, 0x2000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Multiple placeholder unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x4000, 0, 0x1b000, PERMS, HEAP);
    mem.Unmap(0x3000, 0x1c000, HEAP);
    mem.Map(0x3000, 0, 0x20000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Unmap between placeholders", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x2000, 0x4000, HEAP);
    mem.Map(0x2000, 0, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Unmap to origin", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x4000, 0x4000, HEAP);
    mem.Map(0, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Unmap to right", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x8000, 0x4000, HEAP);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Partial right unmap check bindings", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x4000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x1000] = 17;

    mem.Unmap(0x6000, 0x2000, HEAP);

    REQUIRE(ptr[0x1000] == 17);
}

TEST_CASE("HostMemory: Partial left unmap check bindings", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x4000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x3000] = 19;
    ptr[0x3fff] = 12;

    mem.Unmap(0x4000, 0x2000, HEAP);

    REQUIRE(ptr[0x3000] == 19);
    REQUIRE(ptr[0x3fff] == 12);
}

TEST_CASE("HostMemory: Partial middle unmap check bindings", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x4000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x0000] = 19;
    ptr[0x3fff] = 12;

    mem.Unmap(0x1000, 0x2000, HEAP);

    REQUIRE(ptr[0x0000] == 19);
    REQUIRE(ptr[0x3fff] == 12);
}

TEST_CASE("HostMemory: Partial sparse middle unmap and check bindings", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x2000, PERMS, HEAP);
    mem.Map(0x6000, 0x20000, 0x2000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x0000] = 19;
    ptr[0x3fff] = 12;

    mem.Unmap(0x5000, 0x2000, HEAP);

    REQUIRE(ptr[0x0000] == 19);
    REQUIRE(ptr[0x3fff] == 12);
}

#if defined(__linux__)
TEST_CASE("HeapTracker: Fragmented protection preserves backing and bounds", "[common]") {
    HostMemory mem(64_KiB, 512_MiB);
    Common::HeapTracker tracker{mem};
    const auto permissions_at = [&](size_t offset) {
        const auto address = reinterpret_cast<uintptr_t>(mem.VirtualBasePointer() + offset);
        std::ifstream maps{"/proc/self/maps"};
        REQUIRE(maps.is_open());
        std::string line;
        while (std::getline(maps, line)) {
            unsigned long long begin{}, end{};
            char permissions[5]{};
            if (std::sscanf(line.c_str(), "%llx-%llx %4s", &begin, &end, permissions) == 3 &&
                begin <= address && address < end) {
                return std::string{permissions, 3};
            }
        }
        return std::string{};
    };

    tracker.Map(0x1000, 0x1000, 0x3000, PERMS, true);
    tracker.Map(0x4000, 0x8000, 0x1000, PERMS, false);
    size_t extra_maps = 0;
    SCOPE_EXIT {
        tracker.Unmap(0x1000, 0x3000, true);
        tracker.Unmap(0x4000, 0x1000, false);
        if (extra_maps != 0) {
            tracker.Unmap(0x10000, extra_maps * 0x1000, true);
        }
    };
    auto* const backing = mem.BackingBasePointer();
    volatile u8* const data = mem.VirtualBasePointer();
    backing[0x1000] = 11;
    backing[0x2000] = 22;
    backing[0x3000] = 33;
    backing[0x8000] = 44;

    SECTION("Resident fragments and partial request boundaries") {
        // Split one resident mapping into three independently protected fragments.
        tracker.Protect(0x2000, 0x1000, Common::MemoryPermission{});
        REQUIRE(permissions_at(0x1000) == "rw-");
        REQUIRE(permissions_at(0x2000) == "---");
        REQUIRE(permissions_at(0x3000) == "rw-");
        tracker.Protect(0x1000, 0x3000, Common::MemoryPermission::Read);
        REQUIRE(permissions_at(0x1000) == "r--");
        REQUIRE(permissions_at(0x2000) == "r--");
        REQUIRE(permissions_at(0x3000) == "r--");
        REQUIRE(permissions_at(0x4000) == "rw-");

        tracker.Protect(0x2000, 0x2000, PERMS);
        REQUIRE(permissions_at(0x1000) == "r--");
        REQUIRE(permissions_at(0x2000) == "rw-");
        REQUIRE(permissions_at(0x3000) == "rw-");
        data[0x2000] = 66;
        REQUIRE(backing[0x2000] == 66);

        // The ordinary mapped tail is eligible too, despite having no heap-map node.
        tracker.Protect(0x1000, 0x4000, Common::MemoryPermission::Read);
        REQUIRE(permissions_at(0x4000) == "r--");
        REQUIRE(data[0x1000] == 11);
        REQUIRE(data[0x2000] == 66);
        REQUIRE(data[0x3000] == 33);
        REQUIRE(data[0x4000] == 44);
    }

    SECTION("Nonresident spans retain deferred permissions and backing") {
        tracker.Protect(0x2000, 0x1000, PERMS);
        // Exercise real eviction without changing the process/system mapping budget.
        while (extra_maps < 65536 && permissions_at(0x1000) != "---") {
            for (size_t i = 0; i < 256; ++i) {
                tracker.Map(0x10000 + extra_maps * 0x1000, 0, 0x1000, PERMS, true);
                ++extra_maps;
            }
        }
        if (permissions_at(0x1000) != "---") {
            SKIP("Resident budget exceeds the bounded eviction fixture");
        }
        REQUIRE(permissions_at(0x2000) == "---");
        REQUIRE(permissions_at(0x3000) == "---");
        REQUIRE(tracker.DeferredMapSeparateHeap(size_t{0x1000}));
        REQUIRE(tracker.DeferredMapSeparateHeap(size_t{0x3000}));

        tracker.Protect(0x1000, 0x3000, Common::MemoryPermission{});
        tracker.Protect(0x1000, 0x3000, Common::MemoryPermission::Read);
        REQUIRE(permissions_at(0x1000) == "r--");
        REQUIRE(permissions_at(0x2000) == "---");
        REQUIRE(permissions_at(0x3000) == "r--");
        REQUIRE(permissions_at(0x4000) == "rw-");
        REQUIRE(data[0x1000] == 11);
        REQUIRE(data[0x3000] == 33);

        tracker.Protect(0x2000, 0x1000, PERMS);
        REQUIRE(permissions_at(0x2000) == "---");
        REQUIRE(tracker.DeferredMapSeparateHeap(size_t{0x2000}));
        REQUIRE(permissions_at(0x2000) == "rw-");
        REQUIRE(data[0x2000] == 22);
        data[0x2000] = 66;
        REQUIRE(backing[0x2000] == 66);
        REQUIRE(data[0x4000] == 44);
    }
}
#endif
