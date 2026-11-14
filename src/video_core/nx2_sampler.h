// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef __linux__

#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon::Nx2Sampler {

// EDEN_NX2_SAMPLER=1 (Linux, diagnostic): signals the GPU front ("GPU") and back ("GPUBack")
// threads 500 times a second, records where they were, and every 20 s logs the most frequent
// leaf functions and call chains as "module+offset", for `addr2line -f -C -e <binary>`. A thread
// sleeping in a wait shows up in the wait function, so the shares are of wall time.
class Sampler {
public:
    static void MaybeStart() {
        static const bool enabled = [] {
            const char* value = std::getenv("EDEN_NX2_SAMPLER");
            return value != nullptr && value[0] == '1';
        }();
        static std::once_flag once;
        if (enabled) {
            std::call_once(once, [] { Get().Start(); });
        }
    }

private:
    static constexpr int Depth = 12;
    static constexpr size_t RingSize = size_t{1} << 17;

    struct Sample {
        std::array<void*, Depth> pc;
        s32 depth;
        s32 who;
    };

    static Sampler& Get() {
        static Sampler instance;
        return instance;
    }

    static void Handler(int, siginfo_t*, [[maybe_unused]] void* raw_context) {
        Sampler& self = Get();
        const s32 who = self.who_of_tid.load(std::memory_order_relaxed);
        (void)who;
#if defined(__ANDROID__) && defined(__aarch64__)
        // Unwinding from here crashed inside the GPU driver's frames (libunwind's DWARF step):
        // the interrupted instruction and its return address only.
        {
            const auto& context = static_cast<ucontext_t*>(raw_context)->uc_mcontext;
            const size_t slot = self.head.fetch_add(1, std::memory_order_relaxed) % RingSize;
            Sample& sample = self.ring[slot];
            sample.depth = 2;
            sample.pc[0] = reinterpret_cast<void*>(context.pc);
            sample.pc[1] = reinterpret_cast<void*>(context.regs[30]);
            // The callers above, along the frame pointers (kept on arm64), inside this thread's
            // stack only.
            static thread_local uintptr_t stack_low{};
            static thread_local uintptr_t stack_high{};
            if (stack_high == 0) {
                pthread_attr_t attributes;
                if (pthread_getattr_np(pthread_self(), &attributes) == 0) {
                    void* base{};
                    size_t size{};
                    pthread_attr_getstack(&attributes, &base, &size);
                    stack_low = reinterpret_cast<uintptr_t>(base);
                    stack_high = stack_low + size;
                    pthread_attr_destroy(&attributes);
                }
            }
            uintptr_t frame = context.regs[29];
            const uintptr_t stack_pointer = context.sp;
            while (sample.depth < Depth && frame >= stack_pointer && frame >= stack_low &&
                   frame + 16 <= stack_high && (frame & 15) == 0) {
                const uintptr_t next = reinterpret_cast<const uintptr_t*>(frame)[0];
                const uintptr_t caller = reinterpret_cast<const uintptr_t*>(frame)[1];
                if (caller == 0) {
                    break;
                }
                sample.pc[sample.depth++] = reinterpret_cast<void*>(caller);
                if (next <= frame) {
                    break;
                }
                frame = next;
            }
            sample.who = static_cast<s32>(syscall(SYS_gettid));
            return;
        }
#else
        void* frames[Depth + 2];
        const int n = backtrace(frames, Depth + 2);
        const size_t slot = self.head.fetch_add(1, std::memory_order_relaxed) % RingSize;
        Sample& sample = self.ring[slot];
        sample.depth = n > 2 ? n - 2 : 0; // The handler and the signal trampoline
        for (int i = 0; i < sample.depth; ++i) {
            sample.pc[i] = frames[i + 2];
        }
        sample.who = static_cast<s32>(syscall(SYS_gettid));
#endif
    }

    void Start() {
#if !(defined(__ANDROID__) && defined(__aarch64__))
        void* warm[4];
        backtrace(warm, 4); // Loads libgcc outside a signal handler
#endif
        struct sigaction action{};
        action.sa_sigaction = &Handler;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask);
        sigaction(SIGPROF, &action, nullptr);
        ring.resize(RingSize);
        thread = std::thread([this] { Run(); });
        thread.detach();
        LOG_INFO(HW_GPU, "nx2 sampler: sampling the GPU front and back threads at 500 Hz");
    }

    static std::string Where(void* pc) {
        Dl_info info{};
        if (dladdr(pc, &info) == 0 || info.dli_fbase == nullptr) {
            return "??";
        }
        const char* slash = info.dli_fname != nullptr ? std::strrchr(info.dli_fname, '/') : nullptr;
        const char* module = slash != nullptr ? slash + 1 : (info.dli_fname != nullptr ? info.dli_fname : "?");
        char text[160];
        if (info.dli_sname != nullptr && std::strstr(module, "eden") == nullptr &&
            std::strstr(module, "yuzu") == nullptr &&
            std::strstr(module, "eglantine") == nullptr) {
            std::snprintf(text, sizeof(text), "%s:%s", module, info.dli_sname);
        } else {
            std::snprintf(text, sizeof(text), "%s+%#lx", module,
                          static_cast<unsigned long>(reinterpret_cast<char*>(pc) -
                                                     static_cast<char*>(info.dli_fbase)));
        }
        return text;
    }

    std::vector<std::pair<s32, std::string>> Targets() const {
        std::vector<std::pair<s32, std::string>> found;
        const pid_t pid = getpid();
        char path[96];
        std::snprintf(path, sizeof(path), "/proc/%d/task", pid);
        // Scan the task directory for the thread names.
        FILE* list = popen(("ls " + std::string(path)).c_str(), "r");
        if (list == nullptr) {
            return found;
        }
        char line[64];
        while (std::fgets(line, sizeof(line), list) != nullptr) {
            const int tid = std::atoi(line);
            std::snprintf(path, sizeof(path), "/proc/%d/task/%d/comm", pid, tid);
            if (FILE* comm = std::fopen(path, "r")) {
                char name[32]{};
                if (std::fgets(name, sizeof(name), comm) != nullptr) {
                    name[std::strcspn(name, "\n")] = 0;
                    if (std::strcmp(name, "GPU") == 0 || std::strcmp(name, "GPUBack") == 0) {
                        found.emplace_back(tid, name);
                    }
                }
                std::fclose(comm);
            }
        }
        pclose(list);
        return found;
    }

    void Run() {
        prctl(PR_SET_NAME, "nx2sampler", 0, 0, 0); // It inherited "GPU" and would sample itself
        using clock = std::chrono::steady_clock;
        std::vector<std::pair<s32, std::string>> targets;
        std::map<s32, std::string> names;
        auto next_scan = clock::now();
        auto next_report = clock::now() + std::chrono::seconds(20);
        size_t consumed = 0;
        std::map<std::string, u64> leaf[2];
        std::map<std::string, u64> chain[2];
        u64 total[2]{};
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (clock::now() >= next_scan) {
                targets = Targets();
                for (const auto& [tid, name] : targets) {
                    names[tid] = name;
                }
                next_scan = clock::now() + std::chrono::seconds(2);
            }
            for (const auto& [tid, name] : targets) {
                syscall(SYS_tgkill, getpid(), tid, SIGPROF);
            }
            if (clock::now() < next_report) {
                continue;
            }
            next_report = clock::now() + std::chrono::seconds(20);
            const size_t head = this->head.load(std::memory_order_relaxed);
            for (; consumed < head; ++consumed) {
                const Sample& sample = ring[consumed % RingSize];
                if (sample.depth == 0 || head - consumed > RingSize) {
                    continue;
                }
                const auto it = names.find(sample.who);
                const int side = it != names.end() && it->second == "GPUBack" ? 1 : 0;
                ++total[side];
                ++leaf[side][Where(sample.pc[0])];
                std::string text;
                for (int i = 0; i < std::min<int>(sample.depth, Depth); ++i) {
                    text += (i != 0 ? " < " : "") + Where(sample.pc[i]);
                }
                ++chain[side][text];
            }
            for (int side = 0; side < 2; ++side) {
                std::vector<std::pair<u64, std::string>> top;
                for (const auto& [where, count] : leaf[side]) {
                    top.emplace_back(count, where);
                }
                std::sort(top.rbegin(), top.rend());
                LOG_INFO(HW_GPU, "nx2 sampler {} thread: {} samples in the last 20 s", side ? "back" : "front",
                         total[side]);
                for (size_t i = 0; i < std::min<size_t>(top.size(), 25); ++i) {
                    LOG_INFO(HW_GPU, "nx2 sampler {} leaf {:5.1f}% {}", side ? "back" : "front",
                             100.0 * top[i].first / static_cast<double>(std::max<u64>(total[side], 1)),
                             top[i].second);
                }
                std::vector<std::pair<u64, std::string>> chains;
                for (const auto& [where, count] : chain[side]) {
                    chains.emplace_back(count, where);
                }
                std::sort(chains.rbegin(), chains.rend());
                for (size_t i = 0; i < std::min<size_t>(chains.size(), 3000); ++i) {
                    LOG_INFO(HW_GPU, "nx2 sampler {} chain {} of {}: {}", side ? "back" : "front",
                             chains[i].first, total[side], chains[i].second);
                }
            }
            // Each report covers its own 20 s window.
            for (int side = 0; side < 2; ++side) {
                leaf[side].clear();
                chain[side].clear();
                total[side] = 0;
            }
        }
    }

    std::vector<Sample> ring;
    std::atomic<size_t> head{0};
    std::atomic<s32> who_of_tid{0};
    std::thread thread;
};

} // namespace VideoCommon::Nx2Sampler

#elif defined(_WIN32) && defined(_M_X64)

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon::Nx2Sampler {

// Windows: a thread of this process suspends the GPU front ("GPU") and back ("GPUBack") threads
// every 2 ms, reads their context and walks up to 12 frames with the unwind tables. Same report
// as on Linux: leaf functions and call chains as "module+offset" (resolve with the symbols of an
// unstripped link). Frames in generated code (the CPU and macro JITs) end a chain.
class Sampler {
public:
    static void MaybeStart() {
        static const bool enabled = [] {
            const char* value = std::getenv("EDEN_NX2_SAMPLER");
            return value != nullptr && value[0] == '1';
        }();
        static std::once_flag once;
        if (enabled) {
            std::call_once(once, [] {
                std::thread([] { Run(); }).detach();
                LOG_INFO(HW_GPU, "nx2 sampler: sampling the GPU front and back threads at 500 Hz");
            });
        }
    }

private:
    static constexpr int Depth = 12;

    struct Target {
        HANDLE handle;
        int side; // 0 front, 1 back
    };

    static std::vector<Target> Targets() {
        using Describe = HRESULT(WINAPI*)(HANDLE, PWSTR*);
        static const Describe describe = reinterpret_cast<Describe>(
            GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "GetThreadDescription"));
        std::vector<Target> found;
        if (describe == nullptr) {
            return found;
        }
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return found;
        }
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        const DWORD process = GetCurrentProcessId();
        for (BOOL more = Thread32First(snapshot, &entry); more;
             more = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != process ||
                entry.th32ThreadID == GetCurrentThreadId()) {
                continue;
            }
            const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                                 THREAD_QUERY_LIMITED_INFORMATION,
                                             FALSE, entry.th32ThreadID);
            if (thread == nullptr) {
                continue;
            }
            PWSTR name = nullptr;
            int side = -1;
            if (SUCCEEDED(describe(thread, &name)) && name != nullptr) {
                side = std::wcscmp(name, L"GPU") == 0 ? 0 : std::wcscmp(name, L"GPUBack") == 0 ? 1 : -1;
                LocalFree(name);
            }
            if (side >= 0) {
                found.push_back({thread, side});
            } else {
                CloseHandle(thread);
            }
        }
        CloseHandle(snapshot);
        return found;
    }

    static std::string Where(u64 pc) {
        HMODULE module{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(pc), &module) ||
            module == nullptr) {
            return "??";
        }
        static std::map<HMODULE, std::string> names;
        auto it = names.find(module);
        if (it == names.end()) {
            char path[MAX_PATH]{};
            GetModuleFileNameA(module, path, sizeof(path));
            const char* slash = std::strrchr(path, '\\');
            it = names.emplace(module, slash != nullptr ? slash + 1 : path).first;
        }
        char text[160];
        std::snprintf(text, sizeof(text), "%s+%#llx", it->second.c_str(),
                      static_cast<unsigned long long>(pc - reinterpret_cast<u64>(module)));
        return text;
    }

    static void Run() {
        using clock = std::chrono::steady_clock;
        std::vector<Target> targets;
        auto next_scan = clock::now();
        auto next_report = clock::now() + std::chrono::seconds(20);
        std::map<std::vector<u64>, u64> chains[2];
        u64 total[2]{};
        while (true) {
            Sleep(2);
            if (clock::now() >= next_scan) {
                for (const Target& target : targets) {
                    CloseHandle(target.handle);
                }
                targets = Targets();
                next_scan = clock::now() + std::chrono::seconds(2);
            }
            for (const Target& target : targets) {
                if (SuspendThread(target.handle) == static_cast<DWORD>(-1)) {
                    continue;
                }
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                std::array<u64, Depth> pcs{};
                int depth = 0;
                if (GetThreadContext(target.handle, &context)) {
                    while (depth < Depth && context.Rip != 0) {
                        pcs[depth++] = context.Rip;
                        DWORD64 image_base{};
                        const PRUNTIME_FUNCTION function =
                            RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
                        if (function == nullptr) {
                            break; // generated code or a leaf without unwind data
                        }
                        void* handler_data{};
                        DWORD64 establisher{};
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function,
                                         &context, &handler_data, &establisher, nullptr);
                    }
                }
                ResumeThread(target.handle);
                if (depth > 0) {
                    ++chains[target.side][std::vector<u64>(pcs.begin(), pcs.begin() + depth)];
                    ++total[target.side];
                }
            }
            if (clock::now() < next_report) {
                continue;
            }
            next_report = clock::now() + std::chrono::seconds(20);
            for (int side = 0; side < 2; ++side) {
                LOG_INFO(HW_GPU, "nx2 sampler {} thread: {} samples in the last 20 s",
                         side ? "back" : "front", total[side]);
                std::map<std::string, u64> leaf;
                std::vector<std::pair<u64, std::string>> rows;
                for (const auto& [pcs, count] : chains[side]) {
                    std::string text;
                    for (size_t i = 0; i < pcs.size(); ++i) {
                        const std::string where = Where(pcs[i]);
                        if (i == 0) {
                            leaf[where] += count;
                        } else {
                            text += " < ";
                        }
                        text += where;
                    }
                    rows.emplace_back(count, std::move(text));
                }
                std::vector<std::pair<u64, std::string>> top;
                for (const auto& [where, count] : leaf) {
                    top.emplace_back(count, where);
                }
                std::sort(top.rbegin(), top.rend());
                for (size_t i = 0; i < std::min<size_t>(top.size(), 25); ++i) {
                    LOG_INFO(HW_GPU, "nx2 sampler {} leaf {:5.1f}% {}", side ? "back" : "front",
                             100.0 * static_cast<double>(top[i].first) /
                                 static_cast<double>(std::max<u64>(total[side], 1)),
                             top[i].second);
                }
                std::sort(rows.rbegin(), rows.rend());
                for (size_t i = 0; i < std::min<size_t>(rows.size(), 3000); ++i) {
                    LOG_INFO(HW_GPU, "nx2 sampler {} chain {} of {}: {}", side ? "back" : "front",
                             rows[i].first, total[side], rows[i].second);
                }
                chains[side].clear();
                total[side] = 0;
            }
        }
    }
};

} // namespace VideoCommon::Nx2Sampler

#else
namespace VideoCommon::Nx2Sampler {
struct Sampler {
    static void MaybeStart() {}
};
} // namespace VideoCommon::Nx2Sampler
#endif
