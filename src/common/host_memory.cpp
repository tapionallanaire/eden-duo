// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32

#include <iterator>
#include "common/container/unordered_map.h"
#include <boost/icl/separate_interval_set.hpp>
#include <windows.h>
#include "common/dynamic_library.h"

#else // ^^^ Windows ^^^ vvv POSIX vvv

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <boost/icl/interval_set.hpp>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "common/scope_exit.h"

#if defined(__linux__)
#include <sys/random.h>
#elif defined(__APPLE__)
#include <sys/types.h>
#include <sys/random.h>
#include <mach/vm_map.h>
#include <mach/mach.h>
#elif defined(__FreeBSD__)
#include <sys/shm.h>
#elif defined(__OPENORBIS__)
#include <orbis/libkernel.h>
#endif

// FreeBSD
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
// Solaris 11 and illumos
#ifndef MAP_ALIGNED_SUPER
#define MAP_ALIGNED_SUPER 0
#endif
// macOS
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#endif // ^^^ POSIX ^^^

#include <cstdlib>
#include <utility>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <random>
#include <thread>
#include <string_view>
#include <string>
#include <cstring>
#include <cstdio>
#include <chrono>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/cityhash.h"
#include "common/div_ceil.h"
#include "common/free_region_manager.h"
#include "common/host_memory.h"
#include "common/logging.h"
#include "common/settings.h"

#if defined(__ANDROID__) && __ANDROID_API__ < 30
#include <sys/syscall.h>
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
static int memfd_create(const char* name, unsigned int flags) {
    return syscall(__NR_memfd_create, name, flags);
}
#endif

namespace Common {

[[maybe_unused]] constexpr size_t PageAlignment = 0x1000;
[[maybe_unused]] constexpr size_t HugePageSize = 0x200000;

#ifdef _WIN32

// Manually imported for MinGW compatibility
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif

using PFN_CreateFileMapping2 = _Ret_maybenull_ HANDLE(WINAPI*)(
    _In_ HANDLE File, _In_opt_ SECURITY_ATTRIBUTES* SecurityAttributes, _In_ ULONG DesiredAccess,
    _In_ ULONG PageProtection, _In_ ULONG AllocationAttributes, _In_ ULONG64 MaximumSize,
    _In_opt_ PCWSTR Name,
    _Inout_updates_opt_(ParameterCount) MEM_EXTENDED_PARAMETER* ExtendedParameters,
    _In_ ULONG ParameterCount);

using PFN_VirtualAlloc2 = _Ret_maybenull_ PVOID(WINAPI*)(
    _In_opt_ HANDLE Process, _In_opt_ PVOID BaseAddress, _In_ SIZE_T Size,
    _In_ ULONG AllocationType, _In_ ULONG PageProtection,
    _Inout_updates_opt_(ParameterCount) MEM_EXTENDED_PARAMETER* ExtendedParameters,
    _In_ ULONG ParameterCount);

using PFN_MapViewOfFile3 = _Ret_maybenull_ PVOID(WINAPI*)(
    _In_ HANDLE FileMapping, _In_opt_ HANDLE Process, _In_opt_ PVOID BaseAddress,
    _In_ ULONG64 Offset, _In_ SIZE_T ViewSize, _In_ ULONG AllocationType, _In_ ULONG PageProtection,
    _Inout_updates_opt_(ParameterCount) MEM_EXTENDED_PARAMETER* ExtendedParameters,
    _In_ ULONG ParameterCount);

using PFN_UnmapViewOfFile2 = BOOL(WINAPI*)(_In_ HANDLE Process, _In_ PVOID BaseAddress,
                                           _In_ ULONG UnmapFlags);

template <typename T>
static void GetFuncAddress(Common::DynamicLibrary& dll, const char* name, T& pfn) {
    if (!dll.GetSymbol(name, &pfn)) {
        LOG_CRITICAL(HW_Memory, "Failed to load {}", name);
    }
}

class HostMemory::Impl {
public:
    explicit Impl(size_t backing_size_, size_t virtual_size_)
        : backing_size{backing_size_}
        , virtual_size{virtual_size_}
        , process{GetCurrentProcess()}
        , kernelbase_dll("Kernelbase")
    {}

    bool Init() {
        if (!kernelbase_dll.IsOpen()) {
            LOG_CRITICAL(HW_Memory, "Failed to load Kernelbase.dll");
            return false;
        }
        GetFuncAddress(kernelbase_dll, "CreateFileMapping2", pfn_CreateFileMapping2);
        GetFuncAddress(kernelbase_dll, "VirtualAlloc2", pfn_VirtualAlloc2);
        GetFuncAddress(kernelbase_dll, "MapViewOfFile3", pfn_MapViewOfFile3);
        GetFuncAddress(kernelbase_dll, "UnmapViewOfFile2", pfn_UnmapViewOfFile2);

        if (!pfn_CreateFileMapping2 || !pfn_VirtualAlloc2 || !pfn_MapViewOfFile3 || !pfn_UnmapViewOfFile2) {
            LOG_CRITICAL(HW_Memory, "Failed to find functions for virtual allocs");
            return false;
        }

        // Allocate backing file map
        backing_handle = pfn_CreateFileMapping2(INVALID_HANDLE_VALUE, nullptr, FILE_MAP_WRITE | FILE_MAP_READ, PAGE_READWRITE, SEC_COMMIT, backing_size, nullptr, nullptr, 0);
        if (!backing_handle) {
            LOG_CRITICAL(HW_Memory, "Failed to allocate {} MiB of backing memory", backing_size >> 20);
            return false;
        }
        // Allocate a virtual memory for the backing file map as placeholder
        backing_base = static_cast<u8*>(pfn_VirtualAlloc2(process, nullptr, backing_size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
        if (!backing_base) {
            Release();
            LOG_CRITICAL(HW_Memory, "Failed to reserve {} MiB of virtual memory", backing_size >> 20);
            return false;
        }
        // Map backing placeholder
        void* const ret = pfn_MapViewOfFile3(backing_handle, process, backing_base, 0, backing_size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
        if (ret != backing_base) {
            Release();
            LOG_CRITICAL(HW_Memory, "Failed to map {} MiB of virtual memory", backing_size >> 20);
            return false;
        }
        // Allocate virtual address placeholder
        virtual_base = static_cast<u8*>(pfn_VirtualAlloc2(process, nullptr, virtual_size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
        if (!virtual_base) {
            Release();
            LOG_CRITICAL(HW_Memory, "Failed to reserve {} GiB of virtual memory", virtual_size >> 30);
            return false;
        }
        return true;
    }

    ~Impl() {
        Release();
    }

    void* Allocate(size_t size) {
        auto* ptr = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (ptr == nullptr) {
            LOG_CRITICAL(HW_Memory, "Failed to allocate fallback buffer with size {:#x}, error {}", size, GetLastError());
        }
        return ptr;
    }

    void Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms) {
        std::unique_lock lock{placeholder_mutex};
        if (!IsNiechePlaceholder(virtual_offset, length)) {
            Split(virtual_offset, length);
        }
        ASSERT(placeholders.find({virtual_offset, virtual_offset + length}) == placeholders.end());
        TrackPlaceholder(virtual_offset, host_offset, length);

        MapView(virtual_offset, host_offset, length);
    }

    void Unmap(size_t virtual_offset, size_t length) {
        std::scoped_lock lock{placeholder_mutex};

        // Unmap until there are no more placeholders
        while (UnmapOnePlaceholder(virtual_offset, length)) {
        }
    }

    void Protect(size_t virtual_offset, size_t length, bool read, bool write, bool execute) {
        DWORD new_flags{};
        if (read && write) {
            new_flags = PAGE_READWRITE;
        } else if (read && !write) {
            new_flags = PAGE_READONLY;
        } else if (!read && !write) {
            new_flags = PAGE_NOACCESS;
        } else {
            UNIMPLEMENTED_MSG("Protection flag combination read={} write={}", read, write);
        }
        const size_t virtual_end = virtual_offset + length;

        std::scoped_lock lock{placeholder_mutex};
        auto [it, end] = placeholders.equal_range({virtual_offset, virtual_end});
        while (it != end) {
            const size_t offset = (std::max)(it->lower(), virtual_offset);
            const size_t protect_length = (std::min)(it->upper(), virtual_end) - offset;
            DWORD old_flags{};
            if (!VirtualProtect(virtual_base + offset, protect_length, new_flags, &old_flags)) {
                LOG_CRITICAL(HW_Memory, "Failed to change virtual memory protect rules");
            }
            ++it;
        }
    }

    void EnableDirectMappedAddress() {
        // TODO
        UNREACHABLE();
    }

    const size_t backing_size; ///< Size of the backing memory in bytes
    const size_t virtual_size; ///< Size of the virtual address placeholder in bytes

    u8* backing_base{};
    u8* virtual_base{};

private:
    /// Release all resources in the object
    void Release() {
        if (!placeholders.empty()) {
            for (const auto& placeholder : placeholders) {
                if (!pfn_UnmapViewOfFile2(process, virtual_base + placeholder.lower(),
                                          MEM_PRESERVE_PLACEHOLDER)) {
                    LOG_CRITICAL(HW_Memory, "Failed to unmap virtual memory placeholder");
                }
            }
            Coalesce(0, virtual_size);
        }
        if (virtual_base) {
            if (!VirtualFree(virtual_base, 0, MEM_RELEASE)) {
                LOG_CRITICAL(HW_Memory, "Failed to free virtual memory");
            }
        }
        if (backing_base) {
            if (!pfn_UnmapViewOfFile2(process, backing_base, MEM_PRESERVE_PLACEHOLDER)) {
                LOG_CRITICAL(HW_Memory, "Failed to unmap backing memory placeholder");
            }
            if (!VirtualFreeEx(process, backing_base, 0, MEM_RELEASE)) {
                LOG_CRITICAL(HW_Memory, "Failed to free backing memory");
            }
        }
        if (!CloseHandle(backing_handle)) {
            LOG_CRITICAL(HW_Memory, "Failed to free backing memory file handle");
        }
    }

    /// Unmap one placeholder in the given range (partial unmaps are supported)
    /// Return true when there are no more placeholders to unmap
    bool UnmapOnePlaceholder(size_t virtual_offset, size_t length) {
        const auto it = placeholders.find({virtual_offset, virtual_offset + length});
        const auto begin = placeholders.begin();
        const auto end = placeholders.end();
        if (it == end) {
            return false;
        }
        const size_t placeholder_begin = it->lower();
        const size_t placeholder_end = it->upper();
        const size_t unmap_begin = (std::max)(virtual_offset, placeholder_begin);
        const size_t unmap_end = (std::min)(virtual_offset + length, placeholder_end);
        ASSERT(unmap_begin >= placeholder_begin && unmap_begin < placeholder_end);
        ASSERT(unmap_end <= placeholder_end && unmap_end > placeholder_begin);

        const auto host_pointer_it = placeholder_host_pointers.find(placeholder_begin);
        ASSERT(host_pointer_it != placeholder_host_pointers.end());
        const size_t host_offset = host_pointer_it->second;

        const bool split_left = unmap_begin > placeholder_begin;
        const bool split_right = unmap_end < placeholder_end;

        if (!pfn_UnmapViewOfFile2(process, virtual_base + placeholder_begin,
                                  MEM_PRESERVE_PLACEHOLDER)) {
            LOG_CRITICAL(HW_Memory, "Failed to unmap placeholder");
        }
        // If we have to remap memory regions due to partial unmaps, we are in a data race as
        // Windows doesn't support remapping memory without unmapping first. Avoid adding any extra
        // logic within the panic region described below.

        // Panic region, we are in a data race right now
        if (split_left || split_right) {
            Split(unmap_begin, unmap_end - unmap_begin);
        }
        if (split_left) {
            MapView(placeholder_begin, host_offset, unmap_begin - placeholder_begin);
        }
        if (split_right) {
            MapView(unmap_end, host_offset + unmap_end - placeholder_begin,
                    placeholder_end - unmap_end);
        }
        // End panic region

        size_t coalesce_begin = unmap_begin;
        if (!split_left) {
            // Try to coalesce pages to the left
            coalesce_begin = it == begin ? 0 : std::prev(it)->upper();
            if (coalesce_begin != placeholder_begin) {
                Coalesce(coalesce_begin, unmap_end - coalesce_begin);
            }
        }
        if (!split_right) {
            // Try to coalesce pages to the right
            const auto next = std::next(it);
            const size_t next_begin = next == end ? virtual_size : next->lower();
            if (placeholder_end != next_begin) {
                // We can coalesce to the right
                Coalesce(coalesce_begin, next_begin - coalesce_begin);
            }
        }
        // Remove and reinsert placeholder trackers
        UntrackPlaceholder(it);
        if (split_left) {
            TrackPlaceholder(placeholder_begin, host_offset, unmap_begin - placeholder_begin);
        }
        if (split_right) {
            TrackPlaceholder(unmap_end, host_offset + unmap_end - placeholder_begin,
                             placeholder_end - unmap_end);
        }
        return true;
    }

    void MapView(size_t virtual_offset, size_t host_offset, size_t length) {
        if (!pfn_MapViewOfFile3(backing_handle, process, virtual_base + virtual_offset, host_offset,
                                length, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0)) {
            LOG_CRITICAL(HW_Memory, "Failed to map placeholder");
        }
    }

    void Split(size_t virtual_offset, size_t length) {
        if (!VirtualFreeEx(process, reinterpret_cast<LPVOID>(virtual_base + virtual_offset), length,
                           MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            LOG_CRITICAL(HW_Memory, "Failed to split placeholder");
        }
    }

    void Coalesce(size_t virtual_offset, size_t length) {
        if (!VirtualFreeEx(process, reinterpret_cast<LPVOID>(virtual_base + virtual_offset), length,
                           MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
            LOG_CRITICAL(HW_Memory, "Failed to coalesce placeholders");
        }
    }

    void TrackPlaceholder(size_t virtual_offset, size_t host_offset, size_t length) {
        placeholders.insert({virtual_offset, virtual_offset + length});
        placeholder_host_pointers.emplace(virtual_offset, host_offset);
    }

    void UntrackPlaceholder(boost::icl::separate_interval_set<size_t>::iterator it) {
        placeholder_host_pointers.erase(it->lower());
        placeholders.erase(it);
    }

    /// Return true when a given memory region is a "nieche" and the placeholders don't have to be
    /// split.
    bool IsNiechePlaceholder(size_t virtual_offset, size_t length) const {
        const auto it = placeholders.upper_bound({virtual_offset, virtual_offset + length});
        if (it != placeholders.end() && it->lower() == virtual_offset + length) {
            return it == placeholders.begin() ? virtual_offset == 0
                                              : std::prev(it)->upper() == virtual_offset;
        }
        return false;
    }

    HANDLE process{};        ///< Current process handle
    HANDLE backing_handle{}; ///< File based backing memory

    DynamicLibrary kernelbase_dll;
    PFN_CreateFileMapping2 pfn_CreateFileMapping2{};
    PFN_VirtualAlloc2 pfn_VirtualAlloc2{};
    PFN_MapViewOfFile3 pfn_MapViewOfFile3{};
    PFN_UnmapViewOfFile2 pfn_UnmapViewOfFile2{};

    std::mutex placeholder_mutex;                                 ///< Mutex for placeholders
    boost::icl::separate_interval_set<size_t> placeholders;       ///< Mapped placeholders
    ::Common::unordered_map<size_t, size_t> placeholder_host_pointers; ///< Placeholder backing offset
};

#elif defined(__OPENORBIS__) || defined(__managarm__)
// None of the luxuries of POSIX, all of the suffering
// For managarm: see https://github.com/managarm/managarm/issues/1370
#else // ^^^ Windows ^^^ vvv POSIX vvv

#ifndef MAP_NOCORE
#define MAP_NOCORE 0
#endif

#ifdef ARCHITECTURE_arm64

static void* ChooseVirtualBase(size_t virtual_size) {
    constexpr uintptr_t Map39BitSize = (1ULL << 39);
    constexpr uintptr_t Map36BitSize = (1ULL << 36);

    // This is not a cryptographic application, we just want something random.
    std::mt19937_64 rng;

    // We want to ensure we are allocating at an address aligned to the L2 block size.
    // For Qualcomm devices, we must also allocate memory above 36 bits.
    const size_t lower = Map36BitSize / HugePageSize;
    const size_t upper = (Map39BitSize - virtual_size) / HugePageSize;
    const size_t range = upper - lower;

    // Try up to 64 times to allocate memory at random addresses in the range.
    for (int i = 0; i < 64; i++) {
        // Calculate a possible location.
        uintptr_t hint_address = ((rng() % range) + lower) * HugePageSize;

        // Try to map.
        // Note: we may be able to take advantage of MAP_FIXED_NOREPLACE here.
        void* map_pointer =
            mmap(reinterpret_cast<void*>(hint_address), virtual_size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_NOCORE, -1, 0);

        // If we successfully mapped, we're done.
        if (reinterpret_cast<uintptr_t>(map_pointer) == hint_address) {
            return map_pointer;
        }

        // Unmap if necessary, and try again.
        if (map_pointer != MAP_FAILED) {
            munmap(map_pointer, virtual_size);
        }
    }

    return MAP_FAILED;
}

#else

static void* ChooseVirtualBase(size_t virtual_size) {
#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__) || defined(__sun__) || defined(__HAIKU__) || defined(__managarm__) || defined(__AIX__)
    void* virtual_base = mmap(nullptr, virtual_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_ALIGNED_SUPER | MAP_NOCORE, -1, 0);
    if (virtual_base != MAP_FAILED)
        return virtual_base;
#endif
    return mmap(nullptr, virtual_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_NOCORE, -1, 0);
}

#endif

#if defined(__sun__) || defined(__HAIKU__) || defined(__NetBSD__) || defined(__DragonFly__)
/// Most Unices don't have a portable shm_open (AIX, OpenBSD, NetBSD, Solaris 11, OpenIndiana)
/// Portable implementation of shm_open(SHM_ANON, ...) - roughly equivalent but without
/// OS support - may fail sporadically, beware!
static int shm_open_anon(int flags, mode_t mode) {
    char name[16] = "/shm-";
    char *const limit = name + sizeof(name) - 1;
    *limit = '\0';
    char *start = name + strlen(name);
    for (int tries = 0; tries < 4; tries++) {
        struct timespec tv;
        clock_gettime(CLOCK_REALTIME, &tv);
        unsigned long r = (unsigned long)tv.tv_sec + (unsigned long)tv.tv_nsec;
        for (char *fill = start; fill < limit; r /= 8)
            *fill++ = '0' + (r % 8);
        int fd = shm_open(name, flags, mode);
        if (fd != -1) {
            if (shm_unlink(name) == -1) {
                int tmp = errno;
                close(fd);
                errno = tmp;
                return -1;
            }
            return fd;
        }
        if (errno != EEXIST)
            break;
    }
    return -1;
}
#elif defined(__OpenBSD__)
/// Except OpenBSD which explicitly uses shm_mkstemp instead (as a more secure alternative)
static int shm_open_anon(int flags, mode_t mode) {
    char name[16] = "/shm-XXXXXXXXXX";
    int fd;
    if ((fd = shm_mkstemp(name)) == -1)
        return -1;
    if (shm_unlink(name) == -1) {
        int tmp = errno;
        close(fd);
        errno = tmp;
        return -1;
    }
    return fd;
}
#endif

class HostMemory::Impl {
public:
    explicit Impl(size_t backing_size_, size_t virtual_size_)
        : backing_size{backing_size_}
        , virtual_size{virtual_size_}
    {}

#ifdef __linux__
    bool PunchHole(size_t offset, size_t length) {
        return fd >= 0 &&
               fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(offset),
                         static_cast<off_t>(length)) == 0;
    }
#endif

    bool Init() {
        long page_size = sysconf(_SC_PAGESIZE);
        ASSERT_MSG(page_size == 0x1000, "page size {:#x} is incompatible with 4K paging", page_size);
        // Backing memory initialization
#if defined(__sun__) || defined(__HAIKU__) || defined(__NetBSD__) || defined(__DragonFly__)
        fd = shm_open_anon(O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
#elif defined(__OpenBSD__)
        fd = shm_open_anon(O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
#elif defined(__FreeBSD__)
        fd = shm_open(SHM_ANON, O_RDWR, 0600);
#elif defined(__APPLE__) || defined(__managarm__)
        // macOS doesn't have memfd_create, use anonymous temporary file
        char template_path[] = "/tmp/eden_mem_XXXXXX";
        fd = mkstemp(template_path);
        if (fd >= 0) {
            unlink(template_path);
        }
#else
        fd = memfd_create("HostMemory", 0);
#endif
        bool use_anon = false;
        if (fd <= 0) {
            LOG_WARNING(Common_Memory, "memfd_create: {}", strerror(errno));
            use_anon = true;
        }
        if (!use_anon) {
            // Defined to extend the file with zeros
            int ret = ftruncate(fd, backing_size);
            if (ret != 0) {
                LOG_WARNING(Common_Memory, "ftruncate: {} (likely out-of-emory)", strerror(errno));
                use_anon = true;
            }
        }
        if (use_anon) {
            LOG_WARNING(Common_Memory, "Using private mappings instead of shared ones");
            backing_base = static_cast<u8*>(mmap(nullptr, backing_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_NOCORE, -1, 0));
            if (fd > 0) {
                fd = -1;
                close(fd);
            }
        } else {
            backing_base = static_cast<u8*>(mmap(nullptr, backing_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NOCORE, fd, 0));
        }
        if (backing_base == MAP_FAILED) {
            LOG_CRITICAL(HW_Memory, "mmap failed: {}", strerror(errno));
            return false;
        }

        // Virtual memory initialization
        virtual_base = virtual_map_base = static_cast<u8*>(ChooseVirtualBase(virtual_size));
        if (virtual_base == MAP_FAILED) {
            LOG_CRITICAL(HW_Memory, "mmap failed: {}", strerror(errno));
            return false;
        }
#if defined(__linux__)
        madvise(virtual_base, virtual_size, MADV_HUGEPAGE);
#endif
        free_manager.SetAddressSpace(virtual_base, virtual_size);
        return true;
    }

    ~Impl() {
        Release();
    }

    void* Allocate(size_t size) {
        auto* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (ptr == MAP_FAILED) {
            LOG_CRITICAL(HW_Memory, "Failed to allocate fallback buffer with size {:#x}, {}", size, strerror(errno));
        }
        return ptr;
    }

    void Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms) {
        // Intersect the range with our address space.
        AdjustMap(&virtual_offset, &length);

        // We are removing a placeholder.
        free_manager.AllocateBlock(virtual_base + virtual_offset, length);

        // Deduce mapping protection flags.
        int prot_flags = PROT_NONE;
        if (True(perms & MemoryPermission::Read))
            prot_flags |= PROT_READ;
        if (True(perms & MemoryPermission::Write))
            prot_flags |= PROT_WRITE;
#ifdef ARCHITECTURE_arm64
        if (True(perms & MemoryPermission::Execute))
            prot_flags |= PROT_EXEC;
#endif
        int flags = (fd >= 0 ? MAP_SHARED : MAP_PRIVATE) | MAP_FIXED;
#ifdef __ANDROID__
        {
            // Pieces that live in the cold file are mapped from it.
            std::scoped_lock lock{cold_mutex};
            const auto range = boost::icl::interval<size_t>::right_open(host_offset,
                                                                        host_offset + length);
            if (cold_fd >= 0 && boost::icl::intersects(cold_ranges, range)) {
                const auto map_piece = [&](int source, size_t begin, size_t end) {
                    if (begin >= end) {
                        return;
                    }
                    void* piece = mmap(virtual_base + virtual_offset + (begin - host_offset),
                                       end - begin, prot_flags, flags, source,
                                       static_cast<off_t>(begin));
                    ASSERT_MSG(piece != MAP_FAILED, "mmap: {} {}", strerror(errno), source);
                };
                size_t cursor = host_offset;
                const boost::icl::interval_set<size_t> hit = cold_ranges & range;
                for (const auto& piece : hit) {
                    map_piece(fd, cursor, piece.lower());
                    map_piece(cold_fd, piece.lower(), piece.upper());
                    cursor = piece.upper();
                }
                map_piece(fd, cursor, host_offset + length);
                return;
            }
        }
#endif
        void* ret = mmap(virtual_base + virtual_offset, length, prot_flags, flags, fd, host_offset);
        ASSERT_MSG(ret != MAP_FAILED, "mmap: {} {}", strerror(errno), fd);
    }

    void Unmap(size_t virtual_offset, size_t length) {
        // The method name is wrong. We're still talking about the virtual range.
        // We don't want to unmap, we want to reserve this memory.

        // Intersect the range with our address space.
        AdjustMap(&virtual_offset, &length);

        // Merge with any adjacent placeholder mappings.
        auto [merged_pointer, merged_size] =
            free_manager.FreeBlock(virtual_base + virtual_offset, length);

        void* ret = mmap(merged_pointer, merged_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        ASSERT_MSG(ret != MAP_FAILED, "mmap: {}", strerror(errno));
    }

    void Protect(size_t virtual_offset, size_t length, bool read, bool write, bool execute) {
        // Intersect the range with our address space.
        AdjustMap(&virtual_offset, &length);

        int flags = PROT_NONE;
        if (read) {
            flags |= PROT_READ;
        }
        if (write) {
            flags |= PROT_WRITE;
        }
#ifdef HAS_NCE
        if (execute) {
            flags |= PROT_EXEC;
        }
#endif
        int ret = mprotect(virtual_base + virtual_offset, length, flags);
        ASSERT_MSG(ret == 0, "mprotect failed: {}", strerror(errno));
    }

    void EnableDirectMappedAddress() {
        virtual_base = nullptr;
    }

    const size_t backing_size; ///< Size of the backing memory in bytes
    const size_t virtual_size; ///< Size of the virtual address placeholder in bytes

    u8* backing_base{reinterpret_cast<u8*>(MAP_FAILED)};
    u8* virtual_base{reinterpret_cast<u8*>(MAP_FAILED)};
    u8* virtual_map_base{reinterpret_cast<u8*>(MAP_FAILED)};

private:
    /// Release all resources in the object
    void Release() {
        if (virtual_map_base != MAP_FAILED) {
            int ret = munmap(virtual_map_base, virtual_size);
            ASSERT_MSG(ret == 0, "munmap failed: {}", strerror(errno));
        }

        if (backing_base != MAP_FAILED) {
            int ret = munmap(backing_base, backing_size);
            ASSERT_MSG(ret == 0, "munmap failed: {}", strerror(errno));
        }

        if (fd != -1) {
            int ret = close(fd);
            ASSERT_MSG(ret == 0, "close failed: {}", strerror(errno));
        }
    }

    void AdjustMap(size_t* virtual_offset, size_t* length) {
        if (virtual_base != nullptr) {
            return;
        }

        // If we are direct mapped, we want to make sure we are operating on a region
        // that is in range of our virtual mapping.
        size_t intended_start = *virtual_offset;
        size_t intended_end = intended_start + *length;
        size_t address_space_start = reinterpret_cast<size_t>(virtual_map_base);
        size_t address_space_end = address_space_start + virtual_size;

        if (address_space_start > intended_end || intended_start > address_space_end) {
            *virtual_offset = 0;
            *length = 0;
        } else {
            *virtual_offset = (std::max)(intended_start, address_space_start);
            *length = (std::min)(intended_end, address_space_end) - *virtual_offset;
        }
    }

    int fd{-1}; // memfd file descriptor, -1 is the error value of memfd_create
    FreeRegionManager free_manager{};

#ifdef __ANDROID__
public:
    // ---- Cold store: guest memory that stopped changing lives in a file instead of the memfd.
    //
    // A phone has no swap but zram, which is RAM, and the data that goes cold here (texture pools
    // the game filled once: 2 GB in Ocarina of Time) is already compressed. MADV_PAGEOUT does
    // nothing for it either: every guest page is mapped twice (backing view and guest view) and
    // the kernel only pages out pages mapped once. In a file the pages are ordinary page cache:
    // written back once, then dropped whenever RAM is short and read again on use.
    //
    // A physical range is either in the memfd or in the cold file, at the same offset in both.
    // cold_ranges says which; Map consults it, so every view of a page agrees.
    static constexpr size_t ColdBlock = size_t{64} << 10;
    /// Smaller runs cost more mappings than they save.
    static constexpr size_t ColdMinRun = size_t{1} << 20;
    /// One move holds the lock this long.
    static constexpr size_t ColdMaxRun = size_t{32} << 20;
    static constexpr size_t ColdCallBudget = size_t{256} << 20;

    struct ColdView {
        u8* address;
        size_t length;
        size_t file_offset;
        int prot;
    };

    bool OpenColdFile() {
        if (cold_tried) {
            return cold_fd >= 0;
        }
        cold_tried = true;
        if (fd < 0) {
            return false;
        }
        // The app's private cache folder: /data/data/<process name>/cache.
        char name[256]{};
        if (FILE* const file = std::fopen("/proc/self/cmdline", "r")) {
            static_cast<void>(std::fread(name, 1, sizeof(name) - 1, file));
            std::fclose(file);
        }
        if (name[0] == 0 || std::strchr(name, '/') != nullptr) {
            return false;
        }
        const std::string path = std::string{"/data/data/"} + name + "/cache/guest-cold.bin";
        const int file = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (file < 0) {
            LOG_WARNING(Common_Memory, "Cold store: cannot create {}: {}", path, strerror(errno));
            return false;
        }
        unlink(path.c_str());
        struct stat memfd_stat {};
        struct stat cold_stat {};
        if (ftruncate(file, static_cast<off_t>(backing_size)) != 0 ||
            fstat(fd, &memfd_stat) != 0 || fstat(file, &cold_stat) != 0) {
            LOG_WARNING(Common_Memory, "Cold store: cannot size the file: {}", strerror(errno));
            close(file);
            return false;
        }
        memfd_inode = memfd_stat.st_ino;
        cold_inode = cold_stat.st_ino;
        cold_fd = file;
        LOG_INFO(Common_Memory, "Cold store: guest memory that stops changing moves to storage");
        return true;
    }

    /// Every shared mapping of the file with this inode, from /proc/self/maps.
    static std::vector<ColdView> ViewsOf(ino_t inode) {
        std::vector<ColdView> views;
        FILE* const maps = std::fopen("/proc/self/maps", "r");
        if (maps == nullptr) {
            return views;
        }
        char line[512];
        while (std::fgets(line, sizeof(line), maps) != nullptr) {
            unsigned long long start{}, end{}, offset{}, node{};
            char perms[8]{};
            if (std::sscanf(line, "%llx-%llx %7s %llx %*s %llu", &start, &end, perms, &offset,
                            &node) != 5 ||
                node != static_cast<unsigned long long>(inode) || perms[3] != 's') {
                continue;
            }
            int prot = PROT_NONE;
            prot |= perms[0] == 'r' ? PROT_READ : 0;
            prot |= perms[1] == 'w' ? PROT_WRITE : 0;
            prot |= perms[2] == 'x' ? PROT_EXEC : 0;
            views.push_back({reinterpret_cast<u8*>(static_cast<uintptr_t>(start)),
                             static_cast<size_t>(end - start), static_cast<size_t>(offset), prot});
        }
        std::fclose(maps);
        return views;
    }

    /// Points every view of [start, start + length) in `views` at `target` (same offset).
    static bool Retarget(const std::vector<ColdView>& views, int target, size_t start,
                         size_t length) {
        bool ok = true;
        for (const ColdView& view : views) {
            const size_t begin = (std::max)(start, view.file_offset);
            const size_t end = (std::min)(start + length, view.file_offset + view.length);
            if (begin >= end) {
                continue;
            }
            if (mmap(view.address + (begin - view.file_offset), end - begin, view.prot,
                     MAP_SHARED | MAP_FIXED, target, static_cast<off_t>(begin)) == MAP_FAILED) {
                LOG_ERROR(Common_Memory, "Cold store: remap failed: {}", strerror(errno));
                ok = false;
            }
        }
        return ok;
    }

    bool MoveRun(const std::vector<ColdView>& views, size_t start, size_t length) {
        struct statvfs space {};
        if (fstatvfs(cold_fd, &space) != 0 ||
            u64{space.f_bavail} * space.f_frsize < u64{length} + (u64{2} << 30)) {
            if (!std::exchange(cold_full_logged, true)) {
                LOG_WARNING(Common_Memory, "Cold store: storage is nearly full, nothing more moves");
            }
            return false;
        }
        // 1. A copy in the file. 2. Every view on the file. 3. What was written to the memfd in
        // between (through a view not switched yet) copied over. 4. The memfd's pages released.
        for (size_t done = 0; done < length;) {
            const ssize_t written = pwrite(cold_fd, backing_base + start + done, length - done,
                                           static_cast<off_t>(start + done));
            if (written <= 0) {
                LOG_WARNING(Common_Memory, "Cold store: write failed: {}", strerror(errno));
                return false;
            }
            done += static_cast<size_t>(written);
        }
        if (!Retarget(views, cold_fd, start, length)) {
            // Views may disagree now: put all of them back on the memfd, which still has it all.
            Retarget(views, fd, start, length);
            return false;
        }
        void* const old = mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, static_cast<off_t>(start));
        if (old != MAP_FAILED) {
            const u8* const before = static_cast<const u8*>(old);
            u8* const now = backing_base + start;
            for (size_t page = 0; page < length; page += 0x1000) {
                if (std::memcmp(before + page, now + page, 0x1000) != 0) {
                    std::memcpy(now + page, before + page, 0x1000);
                }
            }
            munmap(old, length);
        }
        fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(start),
                  static_cast<off_t>(length));
        cold_ranges += boost::icl::interval<size_t>::right_open(start, start + length);
        cold_bytes += length;
        return true;
    }

    /// One pass of the scan, at most every 5 s (any cold hint calls it; the hinted range itself
    /// is not used: the emulator's hints cover a few MiB of the gigabytes that go cold). Hashes
    /// up to ColdScanBudget of resident guest memory from where the last pass stopped; blocks
    /// whose hash equals the one from their previous visit (a full round earlier, 30 s or more)
    /// move to the file in merged runs. Returns the bytes moved.
    size_t MoveColdToFile(size_t, size_t) {
        std::unique_lock scan{cold_scan_mutex, std::try_to_lock};
        if (!scan.owns_lock()) {
            return 0;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - cold_last_pass < std::chrono::seconds{5}) {
            return 0;
        }
        cold_last_pass = now;
        {
            std::scoped_lock lock{cold_mutex};
            if (!OpenColdFile()) {
                return 0;
            }
        }
        const size_t blocks = backing_size / ColdBlock;
        if (cold_hashes.empty()) {
            cold_hashes.assign(blocks, 0);
            cold_state.assign(blocks, 0);
            cold_zero_hash = [] {
                const std::vector<char> zero(ColdBlock, 0);
                return CityHash64(zero.data(), zero.size());
            }();
        }
        // The scan itself runs without cold_mutex: it only reads, and only this thread touches
        // its tables. State: 0 never seen, 1 hashed once, 2 moved.
        std::vector<std::pair<size_t, size_t>> runs;
        size_t hashed = 0, planned = 0, zero = 0, absent = 0;
        size_t run_start = 0, run_length = 0;
        const auto close_run = [&] {
            if (run_length >= ColdMinRun && planned < ColdCallBudget) {
                runs.emplace_back(run_start, run_length);
                planned += run_length;
            }
            run_length = 0;
        };
        static constexpr size_t Window = size_t{8} << 20;
        std::vector<unsigned char> resident(Window / 0x1000);
        size_t visited = 0;
        while (visited < blocks && hashed < ColdScanBudget) {
            const size_t window_start = Common::AlignDown(cold_cursor * ColdBlock, Window);
            if (mincore(backing_base + window_start, Window, resident.data()) != 0) {
                std::fill(resident.begin(), resident.end(), static_cast<unsigned char>(0));
            }
            const size_t window_end = (std::min)(window_start + Window, backing_size);
            for (; cold_cursor * ColdBlock < window_end && visited < blocks; ++cold_cursor, ++visited) {
                const size_t block = cold_cursor * ColdBlock;
                if (cold_state[cold_cursor] == 2) {
                    close_run();
                    continue;
                }
                const unsigned char* const pages = resident.data() + (block - window_start) / 0x1000;
                bool all = true;
                for (size_t page = 0; page < ColdBlock / 0x1000; ++page) {
                    all = all && (pages[page] & 1) != 0;
                }
                if (!all) {
                    // Not in RAM (never touched, a hole, or compressed away already).
                    cold_state[cold_cursor] = 0;
                    ++absent;
                    close_run();
                    continue;
                }
                const u64 hash = CityHash64(reinterpret_cast<const char*>(backing_base + block),
                                            ColdBlock);
                hashed += ColdBlock;
                const bool unchanged = cold_state[cold_cursor] == 1 && cold_hashes[cold_cursor] == hash;
                cold_hashes[cold_cursor] = hash;
                cold_state[cold_cursor] = 1;
                if (hash == cold_zero_hash) {
                    ++zero;
                    close_run();
                    continue;
                }
                if (!unchanged) {
                    close_run();
                    continue;
                }
                if (run_length != 0 && run_start + run_length != block) {
                    close_run();
                }
                if (run_length == 0) {
                    run_start = block;
                }
                run_length += ColdBlock;
                if (run_length >= ColdMaxRun) {
                    close_run();
                }
            }
            if (cold_cursor >= blocks) {
                cold_cursor = 0;
                close_run();
            }
        }
        close_run();
        size_t moved = 0;
        if (!runs.empty()) {
            const std::vector<ColdView> views = ViewsOf(memfd_inode);
            for (const auto& [start, size] : runs) {
                // The lock is taken per run, so a guest thread mapping memory waits for one run.
                std::scoped_lock lock{cold_mutex};
                if (!MoveRun(views, start, size)) {
                    break;
                }
                for (size_t block = start / ColdBlock; block < (start + size) / ColdBlock; ++block) {
                    cold_state[block] = 2;
                }
                moved += size;
            }
        }
        if (moved != 0 || (cold_passes++ % 6) == 0) {
            LOG_INFO(Common_Memory,
                     "Cold store: pass hashed {} MiB ({} MiB zero, {} MiB not in RAM), moved {} "
                     "MiB to storage, {} MiB there in all",
                     hashed >> 20, (zero * ColdBlock) >> 20, (absent * ColdBlock) >> 20,
                     moved >> 20, cold_bytes >> 20);
        }
        return moved;
    }

    /// Guest memory handed out again (cleared): back to the memfd, whose range is a hole (zero).
    void ReturnFromColdFile(size_t physical_offset, size_t length) {
        std::scoped_lock lock{cold_mutex};
        if (cold_fd < 0) {
            return;
        }
        const auto range = boost::icl::interval<size_t>::right_open(physical_offset,
                                                                    physical_offset + length);
        if (cold_ranges.empty()) {
            return;
        }
        if (!boost::icl::intersects(cold_ranges, range)) {
            return;
        }
        const std::vector<ColdView> views = ViewsOf(cold_inode);
        const boost::icl::interval_set<size_t> hit = cold_ranges & range;
        for (const auto& piece : hit) {
            const size_t start = piece.lower();
            const size_t size = piece.upper() - piece.lower();
            Retarget(views, fd, start, size);
            fallocate(cold_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                      static_cast<off_t>(start), static_cast<off_t>(size));
            cold_bytes -= (std::min)(cold_bytes, size);
            for (size_t block = start / ColdBlock;
                 block < Common::DivCeil(start + size, ColdBlock) && block < cold_state.size();
                 ++block) {
                cold_state[block] = 0;
            }
        }
        cold_ranges -= range;
    }

    int cold_fd{-1};
    bool cold_tried{};
    bool cold_full_logged{};
    ino_t memfd_inode{};
    ino_t cold_inode{};
    size_t cold_bytes{};
    std::mutex cold_mutex;
    boost::icl::interval_set<size_t> cold_ranges;
    static constexpr size_t ColdScanBudget = size_t{768} << 20;
    /// Serializes the scan; cold_mutex guards the ranges and the mappings.
    std::mutex cold_scan_mutex;
    std::vector<u64> cold_hashes;
    std::vector<u8> cold_state;
    u64 cold_zero_hash{};
    size_t cold_cursor{};
    u32 cold_passes{};
    std::chrono::steady_clock::time_point cold_last_pass{};
#endif
};

#endif // ^^^ POSIX ^^^

HostMemory::HostMemory(size_t backing_size_, size_t virtual_size_)
    : backing_size(backing_size_)
    , virtual_size(virtual_size_)
    , num_granules(Common::DivCeil(backing_size_, size_t{1} << GranuleShift))
{
    granules = std::make_unique<std::atomic<u8>[]>(num_granules);
#if defined(__OPENORBIS__) || defined(__managarm__)
    LOG_WARNING(HW_Memory, "Platform doesn't support fastmem");
    backing_base = static_cast<u8*>(mmap(nullptr, backing_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    virtual_base = nullptr;
#else
    // Try to allocate a fastmem arena.
    impl = std::make_unique<HostMemory::Impl>(AlignUp(backing_size, PageAlignment), AlignUp(virtual_size, PageAlignment) + HugePageSize);
    if (impl->Init()) {
        backing_base = impl->backing_base;
        virtual_base = impl->virtual_base;
        if (virtual_base) {
            // Ensure the virtual base is aligned to the L2 block size.
            virtual_base = reinterpret_cast<u8*>(Common::AlignUp(uintptr_t(virtual_base), HugePageSize));
            virtual_base_offset = virtual_base - impl->virtual_base;
        }
    } else {
        LOG_WARNING(HW_Memory, "Platform can support fastmem, but can't create it");
        fallback_buffer = true;
        backing_base = static_cast<u8*>(impl->Allocate(backing_size));
        virtual_base = nullptr;
        impl.reset();
    }
#endif
}

HostMemory::~HostMemory() {
#ifdef _WIN32
    if (fallback_buffer) {
        VirtualFree(backing_base, backing_size, MEM_RELEASE);
    }
#else
    if (fallback_buffer) {
        munmap(backing_base, backing_size);
    }
#endif
}

HostMemory::HostMemory(HostMemory&&) noexcept = default;

HostMemory& HostMemory::operator=(HostMemory&&) noexcept = default;

void HostMemory::Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms, bool separate_heap) {
#if !(defined(__OPENORBIS__) || defined(__managarm__))
    ASSERT(virtual_offset % PageAlignment == 0);
    ASSERT(host_offset % PageAlignment == 0);
    ASSERT(length % PageAlignment == 0);
    ASSERT(virtual_offset + length <= virtual_size);
    ASSERT(host_offset + length <= backing_size);
    if (length == 0 || !virtual_base || !impl) {
        return;
    }
    impl->Map(virtual_offset + virtual_base_offset, host_offset, length, perms);
#endif
}

void HostMemory::Unmap(size_t virtual_offset, size_t length, bool separate_heap) {
#if !(defined(__OPENORBIS__) || defined(__managarm__))
    ASSERT(virtual_offset % PageAlignment == 0);
    ASSERT(length % PageAlignment == 0);
    ASSERT_MSG(virtual_offset <= virtual_size && length <= virtual_size - virtual_offset,
               "Unmap range: offset={:#x}, length={:#x}, limit={:#x}",
               virtual_offset, length, virtual_size);
    if (length == 0 || !virtual_base || !impl) {
        return;
    }
    impl->Unmap(virtual_offset + virtual_base_offset, length);
#endif
}

void HostMemory::Protect(size_t virtual_offset, size_t length, MemoryPermission perm) {
#if !(defined(__OPENORBIS__) || defined(__managarm__))
    ASSERT(virtual_offset % PageAlignment == 0);
    ASSERT(length % PageAlignment == 0);
    ASSERT(virtual_offset + length <= virtual_size);
    if (length == 0 || !virtual_base || !impl) {
        return;
    }
    const bool read = True(perm & MemoryPermission::Read);
    const bool write = True(perm & MemoryPermission::Write);
    const bool execute = True(perm & MemoryPermission::Execute);
    impl->Protect(virtual_offset + virtual_base_offset, length, read, write, execute);
#endif
}

namespace {
// -1: follow the setting; otherwise the overriding Settings::ColdHintMode.
std::atomic<int> g_cold_hint_override{[] {
    const char* const value = std::getenv("EDEN_NX2_COLD_HINT");
    if (value == nullptr || value[0] == '\0') {
        return -1;
    }
    const std::string_view mode{value};
    if (mode == "0" || mode == "off") {
        return static_cast<int>(Settings::ColdHintMode::Off);
    }
    if (mode == "low" || mode == "2") {
        return static_cast<int>(Settings::ColdHintMode::WhenRamIsLow);
    }
    return static_cast<int>(Settings::ColdHintMode::Always);
}()};

const char* ColdHintModeName(Settings::ColdHintMode mode) {
    switch (mode) {
    case Settings::ColdHintMode::Off:
        return "off";
    case Settings::ColdHintMode::WhenRamIsLow:
        return "when RAM is low";
    case Settings::ColdHintMode::Always:
        return "always";
    }
    return "?";
}

bool ReadMemoryIsLow() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        return false;
    }
    const u64 floor = std::max<u64>(u64{1} << 30, status.ullTotalPhys / 10);
    return status.ullAvailPhys < floor;
#elif defined(__linux__)
    const auto read_file = [](const char* path) {
        std::string text;
        if (FILE* const file = std::fopen(path, "r")) {
            char buffer[4096];
            const size_t size = std::fread(buffer, 1, sizeof(buffer) - 1, file);
            std::fclose(file);
            text.assign(buffer, size);
        }
        return text;
    };
    // Some task stalled on memory for over 5% of the last 10 s: the kernel is reclaiming. Our
    // own cgroup first (a memory limit there, as in a container or systemd scope, shows in it
    // and not system-wide), then the whole system.
    const auto stalled = [&](const std::string& path) {
        const std::string pressure = read_file(path.c_str());
        const size_t at = pressure.find("some avg10=");
        return at != std::string::npos && std::strtod(pressure.c_str() + at + 11, nullptr) > 5.0;
    };
    static const std::string cgroup_pressure = [&] {
        // cgroup v2: "0::/path".
        const std::string self = read_file("/proc/self/cgroup");
        const size_t at = self.find("0::");
        if (at == std::string::npos) {
            return std::string{};
        }
        const size_t end = self.find('\n', at);
        return "/sys/fs/cgroup" + self.substr(at + 3, end - at - 3) + "/memory.pressure";
    }();
    if ((!cgroup_pressure.empty() && stalled(cgroup_pressure)) ||
        stalled("/proc/pressure/memory")) {
        return true;
    }
    const std::string meminfo = read_file("/proc/meminfo");
    const auto field = [&](const char* name) -> u64 {
        const size_t at = meminfo.find(name);
        return at == std::string::npos
                   ? 0
                   : std::strtoull(meminfo.c_str() + at + std::strlen(name), nullptr, 10) * 1024;
    };
    const u64 total = field("MemTotal:");
    const u64 available = field("MemAvailable:");
    if (total == 0 || available == 0) {
        return false;
    }
#ifdef __ANDROID__
    // Android's low memory killer acts long before a tenth is left (Odin 2, 16 GB: Ocarina of
    // Time killed with 2 GB available), and a 3D scene takes 5 GB within seconds, too fast to
    // start making room only then: half of RAM counts as low here.
    return available < std::max<u64>(u64{1} << 30, total / 2);
#else
    return available < std::max<u64>(u64{1} << 30, total / 10);
#endif
#else
    return false;
#endif
}
} // Anonymous namespace

Settings::ColdHintMode ColdHintModeInEffect() {
    const int override_value = g_cold_hint_override.load(std::memory_order_relaxed);
    return override_value < 0 ? Settings::values.cold_hint_mode.GetValue()
                              : static_cast<Settings::ColdHintMode>(override_value);
}

void OverrideColdHintMode(Settings::ColdHintMode mode) {
    g_cold_hint_override.store(static_cast<int>(mode), std::memory_order_relaxed);
    LOG_INFO(Common_Memory, "Cold hints: {}", ColdHintModeName(mode));
}

bool MemoryIsLow() {
    using Clock = std::chrono::steady_clock;
    static std::atomic<s64> checked_at{};
    static std::atomic<bool> low{};
    const s64 now = Clock::now().time_since_epoch().count();
    const s64 last = checked_at.load(std::memory_order_relaxed);
    if (last == 0 || now - last >= std::chrono::nanoseconds{std::chrono::seconds{1}}.count()) {
        checked_at.store(now, std::memory_order_relaxed);
        const bool is_low = ReadMemoryIsLow();
        if (is_low != low.exchange(is_low, std::memory_order_relaxed)) {
            LOG_INFO(Common_Memory, "System memory {}", is_low ? "low" : "no longer low");
        }
    }
    return low.load(std::memory_order_relaxed);
}

ColdHintAction HostMemory::HintCold(size_t physical_offset, size_t length,
                                    std::optional<size_t> virtual_offset) const {
    if (length == 0 || backing_base == nullptr) {
        return ColdHintAction::None;
    }
    const Settings::ColdHintMode mode = ColdHintModeInEffect();
    if (mode == Settings::ColdHintMode::Off) {
        return ColdHintAction::None;
    }
    const bool page_out = mode == Settings::ColdHintMode::Always || MemoryIsLow();
#ifdef __ANDROID__
    // MADV_PAGEOUT skips these pages (mapped twice) and zram would not shrink them: what has
    // stopped changing goes to the cold file instead.
    if (page_out && impl) {
        return impl->MoveColdToFile(physical_offset, length) != 0 ? ColdHintAction::PagedOut
                                                                  : ColdHintAction::None;
    }
#endif
#if defined(_WIN32)
    // Windows has no cheap mark: leave the memory alone until it is short.
    if (!page_out) {
        return ColdHintAction::None;
    }
#elif !defined(__linux__) || !defined(MADV_COLD) || !defined(MADV_PAGEOUT)
    return ColdHintAction::None;
#endif
    // Paging out reclaims synchronously (compressing into zram, writing swap, unmapping), which
    // also slows the threads touching nearby memory: work in pieces and pause between them.
    static constexpr size_t Piece = size_t{8} << 20;
    static constexpr auto Budget = std::chrono::milliseconds{2};
    const auto hint = [page_out, length](u8* address) {
        auto slice_start = std::chrono::steady_clock::now();
        for (size_t done = 0; done < length;) {
            const size_t size = std::min(Piece, length - done);
#ifdef _WIN32
            // Unlocking pages that are not locked takes them out of the working set.
            VirtualUnlock(address + done, size);
#elif defined(__linux__) && defined(MADV_COLD) && defined(MADV_PAGEOUT)
            madvise(address + done, size, page_out ? MADV_PAGEOUT : MADV_COLD);
#endif
            done += size;
            if (page_out && std::chrono::steady_clock::now() - slice_start >= Budget) {
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
                slice_start = std::chrono::steady_clock::now();
            }
        }
    };
    hint(backing_base + physical_offset);
    if (virtual_offset && virtual_base != nullptr && *virtual_offset + length <= virtual_size) {
        hint(virtual_base + *virtual_offset);
    }
    return page_out ? ColdHintAction::PagedOut : ColdHintAction::MarkedCold;
}

void HostMemory::ClearBackingRegion(size_t physical_offset, size_t length, u32 fill_value) {
    if (length == 0) {
        return;
    }
    MarkUsed(physical_offset, length);
#ifdef __ANDROID__
    if (impl && length >= PageAlignment && IsAligned(physical_offset, PageAlignment) &&
        IsAligned(length, PageAlignment)) {
        impl->ReturnFromColdFile(physical_offset, length);
    }
#endif
    if (fill_value != 0) {
        std::memset(backing_base + physical_offset, fill_value, length);
        MarkDirty(physical_offset, length);
        return;
    }
#if defined(__linux__)
    // Linux/Android: discard the range outright, whatever the granule state says (it saves the
    // same RAM as the skip below and also holds for memory written without MarkDirty).
    // Remove shared backing pages, not just this mapping's view of their contents.
    // Unaligned clears fall back to memset; split off the edge bytes if a caller needs this fast.
    if (length >= PageAlignment && IsAligned(physical_offset, PageAlignment) &&
        IsAligned(length, PageAlignment) &&
        madvise(backing_base + physical_offset, length, MADV_REMOVE) == 0) {
        return;
    }
    std::memset(backing_base + physical_offset, 0, length);
#else
    // Fresh backing memory is zero and the host gives it no RAM until it's touched: clear only
    // the granules that may hold data (title 1's heap made the host commit every page of it).
    // A granule cleared in full is zero again.
    const size_t end = physical_offset + length;
    for (size_t offset = physical_offset; offset < end;) {
        const size_t index = offset >> GranuleShift;
        const size_t granule_begin = index << GranuleShift;
        const size_t granule_end = granule_begin + (size_t{1} << GranuleShift);
        const size_t clear_end = (std::min)(granule_end, end);
        if ((granules[index].load(std::memory_order_acquire) & GranuleDirty) != 0) {
            std::memset(backing_base + offset, 0, clear_end - offset);
            if (offset == granule_begin && clear_end == granule_end) {
                granules[index].fetch_and(static_cast<u8>(~GranuleDirty), std::memory_order_release);
            }
        }
        offset = clear_end;
    }
#endif
}

void HostMemory::MarkUsed(size_t physical_offset, size_t length) {
    if (length == 0) {
        return;
    }
    const size_t last = (physical_offset + length - 1) >> GranuleShift;
    for (size_t index = physical_offset >> GranuleShift; index <= last; ++index) {
        if ((granules[index].load(std::memory_order_relaxed) & GranuleUsed) == 0) {
            granules[index].fetch_or(GranuleUsed, std::memory_order_release);
        }
    }
}

void HostMemory::MarkDirty(size_t physical_offset, size_t length) {
    if (length == 0) {
        return;
    }
    const size_t last = (physical_offset + length - 1) >> GranuleShift;
    for (size_t index = physical_offset >> GranuleShift; index <= last; ++index) {
        granules[index].fetch_or(GranuleDirty, std::memory_order_release);
    }
}

void HostMemory::ForgetContents() {
    for (size_t index = 0; index < num_granules; ++index) {
        if ((granules[index].load(std::memory_order_relaxed) & GranuleUsed) != 0) {
            granules[index].fetch_or(GranuleDirty, std::memory_order_release);
        }
    }
}

void HostMemory::EnableDirectMappedAddress() {
#if !(defined(__OPENORBIS__) || defined(__managarm__))
    if (impl) {
        impl->EnableDirectMappedAddress();
        virtual_size += reinterpret_cast<uintptr_t>(virtual_base);
    }
#endif
}

} // namespace Common
