// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <format>

#include "common/cityhash.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "common/zstd_compression.h"
#include "core/core.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/loader/loader.h"
#include "video_core/shader_warm/shader_archives.h"
#include "video_core/shader_warm/shader_warm_builder.h"
#include "video_core/shader_warm/warm_records.h"

namespace VideoCommon::ShaderWarm {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view StageReading = "Reading game files";
constexpr std::string_view StageExtracting = "Extracting shaders";
constexpr std::string_view StageConverting = "Converting";
constexpr std::string_view StageWriting = "Writing";
/// Conversions in flight (each holds a copy of its program).
constexpr size_t MaxPendingJobs = 2048;
/// Share of the progress bar for reading and extracting the archives; conversions run alongside
/// and only their tail after the last archive shows in the rest.
constexpr double ExtractionShare = 0.95;

fs::path TitleDir(u64 title_id) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir) /
           std::format("{:016x}", title_id);
}

struct ArchiveFile {
    std::string path; ///< Inside the RomFS, '/' separated (the archive identity)
    FileSys::VirtualFile file;
};

/// Appends every file under `dir` to `out`, recursively, with its '/' separated path.
void ListFiles(const FileSys::VirtualDir& dir, const std::string& prefix,
               std::vector<ArchiveFile>& out) {
    for (const auto& file : dir->GetFiles()) {
        out.push_back({prefix + file->GetName(), file});
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        ListFiles(sub, prefix + sub->GetName() + "/", out);
    }
}

/// The game's RomFS trees in the order the offline pipeline read them: a .nup gives its base
/// and newest update package (both are read, like the exported packages 0 and 1); other
/// formats give the loader's RomFS.
struct GameFiles {
    std::unique_ptr<FileSys::NupContainer> nup;
    std::unique_ptr<Loader::AppLoader> loader;
    std::vector<FileSys::VirtualDir> romfs;
};

/// Opens the game file and collects its RomFS trees into `game`. Returns the error text when the
/// file cannot be opened or has no RomFS.
std::optional<std::string> OpenGame(Core::System& system, const fs::path& game_path,
                                    GameFiles& game, std::stop_token stop) {
    FileSys::VirtualFilesystem filesystem = system.GetFilesystem();
    if (filesystem == nullptr) {
        filesystem = std::make_shared<FileSys::RealVfsFilesystem>();
    }
    const auto file =
        filesystem->OpenFile(Common::FS::PathToUTF8String(game_path), FileSys::OpenMode::Read);
    if (file == nullptr) {
        return "Cannot open the game file";
    }
    if (FileSys::NupContainer::IsNupFile(file)) {
        // Small memory cache: every member is read once.
        game.nup = std::make_unique<FileSys::NupContainer>(file, 128 * 1024 * 1024);
        if (game.nup->GetStatus() != Loader::ResultStatus::Success) {
            return "The game file is not a valid package";
        }
        std::vector<size_t> packages;
        if (const auto base = game.nup->FindBasePackage()) {
            packages.push_back(*base);
        }
        if (const auto update = game.nup->FindBestUpdatePackage()) {
            packages.push_back(*update);
        }
        std::sort(packages.begin(), packages.end());
        for (const size_t package : packages) {
            if (stop.stop_requested()) {
                return "Stopped";
            }
            if (auto dir = game.nup->GetRomFSDir(package)) {
                game.romfs.push_back(std::move(dir));
            }
        }
    } else {
        game.loader = Loader::GetLoader(system, file);
        if (game.loader == nullptr) {
            return "Unsupported game file format";
        }
        FileSys::VirtualFile romfs;
        if (game.loader->ReadRomFS(romfs) != Loader::ResultStatus::Success || romfs == nullptr) {
            return "Cannot read the game's RomFS (are the keys installed?)";
        }
        if (stop.stop_requested()) {
            return "Stopped";
        }
        if (auto dir = FileSys::ExtractRomFS(romfs)) {
            game.romfs.push_back(std::move(dir));
        }
    }
    if (game.romfs.empty()) {
        return "The game has no RomFS";
    }
    return std::nullopt;
}

struct PairHash {
    u128 a{};
    u128 b{};
    bool operator==(const PairHash&) const = default;
};
struct PairHasher {
    size_t operator()(const PairHash& h) const noexcept {
        return static_cast<size_t>(h.a[0] ^ (h.b[0] * 0x9E3779B97F4A7C15ULL));
    }
};

u128 Hash128(std::span<const u8> bytes) {
    return Common::CityHash128(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

struct Job {
    size_t seq{};
    std::vector<u8> program;
    ComputeControl control{};
    bool graphics{}; ///< A graphics program, lowered for the pipeline list only
    bool geometry{};
    u32 pair_program{}; ///< A vertex or fragment program kept for the pairs: its number, from 1
    bool fragment{};
};

struct JobResult {
    bool ok{};
    bool graphics{};
    std::vector<u8> bytes; ///< The full compute entry
    ComputeKey compute_key{};
    u64 hash{};           ///< StageHash of `code`
    std::vector<u8> code; ///< The lowered code
    u32 pair_program{};
    std::vector<u8> entry; ///< With `pair_program`: PairProgramEntry
};

/// A vertex or fragment program of the game files kept for the pairs file.
struct PairProgram {
    ProgramStage stage{};
    u64 hash{};            ///< The runtime's hash of its code
    std::vector<u8> entry; ///< PairProgramEntry, empty when it did not lower
};

/// What a pairs file holds of a program behind its hash and stage: the shader program header,
/// then the size and bytes of its compressed StageEntry.
std::vector<u8> PairProgramEntry(const LoweredCode& lowered, ProgramStage stage) {
    const std::vector<u8> stage_entry = StageEntry(lowered, stage == ProgramStage::Fragment);
    const std::vector<u8> packed =
        Common::Compression::CompressDataZSTDDefault(stage_entry.data(), stage_entry.size());
    std::vector<u8> entry(lowered.code.begin(), lowered.code.begin() + 0x50);
    const u32 size = static_cast<u32>(packed.size());
    entry.insert(entry.end(), reinterpret_cast<const u8*>(&size),
                 reinterpret_cast<const u8*>(&size) + sizeof(size));
    entry.insert(entry.end(), packed.begin(), packed.end());
    return entry;
}

class Converter {
public:
    /// Starts `threads` workers that convert pushed jobs until `stop` is requested.
    Converter(size_t threads, std::stop_token stop_) : stop{std::move(stop_)} {
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] { Work(); });
        }
    }

    /// Lets the workers finish the queued jobs, then joins them.
    ~Converter() {
        {
            std::scoped_lock lock{mutex};
            closing = true;
        }
        jobs_cv.notify_all();
        for (auto& worker : workers) {
            worker.join();
        }
    }

    void Push(Job job) {
        {
            std::scoped_lock lock{mutex};
            jobs.push_back(std::move(job));
            ++pushed;
        }
        jobs_cv.notify_one();
    }

    /// Next result in submission order; waits when `wait` is set, else empty when not ready.
    std::optional<JobResult> Next(bool wait) {
        std::unique_lock lock{mutex};
        for (;;) {
            const auto it = results.find(next);
            if (it != results.end()) {
                JobResult result = std::move(it->second);
                results.erase(it);
                ++next;
                return result;
            }
            if (!wait || next == pushed || stop.stop_requested()) {
                return std::nullopt;
            }
            results_cv.wait_for(lock, std::chrono::milliseconds{100});
        }
    }

    size_t Pending() {
        std::scoped_lock lock{mutex};
        return pushed - next;
    }

private:
    /// Worker loop: takes jobs in order, converts each one (skipped once stopping) and files the
    /// result under its sequence number. Returns once closing with no job left.
    void Work() {
        Common::SetCurrentThreadName("ShaderWarmBuild");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
        for (;;) {
            Job job;
            {
                std::unique_lock lock{mutex};
                jobs_cv.wait(lock, [this] { return closing || !jobs.empty(); });
                if (jobs.empty()) {
                    return;
                }
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            JobResult result;
            if (!stop.stop_requested()) {
                result = Convert(job);
            }
            {
                std::scoped_lock lock{mutex};
                results.emplace(job.seq, std::move(result));
            }
            results_cv.notify_all();
        }
    }

    /// Lowers one program and makes its compute entry. The result is not ok when lowering or
    /// conversion failed.
    static JobResult Convert(const Job& job) {
        JobResult result;
        result.graphics = job.graphics;
        result.pair_program = job.pair_program;
        try {
            LoweredCode lowered;
            if (const auto error =
                    job.graphics ? LowerGraphicsProgram(job.program, job.geometry, lowered)
                                 : LowerProgram(job.program, lowered)) {
                LOG_DEBUG(Render_Vulkan, "Shader precache: program not converted: {}", *error);
                return result;
            }
            if (!job.graphics) {
                result.bytes = ComputeEntry(lowered, job.control, result.compute_key);
            }
            result.hash = StageHash(lowered.code);
            if (job.pair_program != 0) {
                result.entry = PairProgramEntry(
                    lowered, job.fragment ? ProgramStage::Fragment : ProgramStage::Vertex);
            }
            result.code = std::move(lowered.code);
            result.ok = true;
        } catch (const std::exception& e) {
            LOG_DEBUG(Render_Vulkan, "Shader precache: conversion failed: {}", e.what());
        }
        return result;
    }

    std::stop_token stop;
    std::mutex mutex;
    std::condition_variable jobs_cv;
    std::condition_variable results_cv;
    std::deque<Job> jobs;
    std::map<size_t, JobResult> results;
    size_t pushed{};
    size_t next{};
    bool closing{};
    std::vector<std::thread> workers;
};

template <typename T>
void Write(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

void WriteHeader(std::ofstream& out) {
    out.write("yuzucach", 8);
    Write(out, WarmCacheVersion);
}

/// Records `entries` for the finished precache file at `warm_file` (see WarmCountPath).
void WriteWarmCount(const fs::path& warm_file, u64 entries) {
    std::error_code ec;
    const auto size = fs::file_size(warm_file, ec);
    if (ec) {
        return;
    }
    std::ofstream out(WarmCountPath(warm_file), std::ios::trunc);
    out << entries << ' ' << size << ' ' << WarmBuilderVersion << '\n';
}

std::vector<u8> ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// Writes a pipeline cache file through a temporary one; `fill` writes its records.
void WriteCacheFile(const fs::path& path, const std::function<void(std::ofstream&)>& fill) {
    fs::path tmp = path;
    tmp += ".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    WriteHeader(out);
    fill(out);
    out.close();
    std::error_code ec;
    if (out) {
        fs::rename(tmp, path, ec);
    }
    if (!out || ec) {
        fs::remove(tmp, ec);
        LOG_WARNING(Render_Vulkan, "Shader precache: cannot write {}",
                    Common::FS::PathToUTF8String(path));
    }
}

bool EnvOn(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] == '1';
}

} // Anonymous namespace

fs::path ComputeWarmPath(u64 title_id) {
    return TitleDir(title_id) / "warm-compute.bin";
}

fs::path PlanPath(u64 title_id) {
    return TitleDir(title_id) / "plan.bin";
}

fs::path GraphicsWarmPath(u64 title_id) {
    return TitleDir(title_id) / "warm-graphics.bin";
}

fs::path PeanutsListPath(u64 title_id) {
    return TitleDir(title_id) / "peanuts.tsv.zst";
}

fs::path PairsWarmPath(u64 title_id) {
    return TitleDir(title_id) / "warm-pairs.bin";
}

fs::path PairsProgressPath(u64 title_id) {
    return TitleDir(title_id) / "warm-pairs.built";
}

fs::path WarmMarkerPath(const fs::path& warm_file) {
    fs::path marker = warm_file;
    marker.replace_extension(".done");
    return marker;
}

fs::path WarmCountPath(const fs::path& warm_file) {
    fs::path count = warm_file;
    count.replace_extension(".count");
    return count;
}

u64 ReadWarmCount(const fs::path& warm_file) {
    std::ifstream in(WarmCountPath(warm_file));
    u64 entries{};
    u64 size{};
    if (!(in >> entries >> size)) {
        return 0;
    }
    std::error_code ec;
    const auto actual = fs::file_size(warm_file, ec);
    return !ec && actual == size ? entries : 0;
}

std::optional<WarmStamp> CurrentWarmStamp(const fs::path& warm_file, u64 entries, u32 driver,
                                          u32 device_id, u32 cache_version) {
    std::error_code ec;
    const auto size = fs::file_size(warm_file, ec);
    if (ec) {
        return std::nullopt;
    }
    const auto time = fs::last_write_time(warm_file, ec);
    if (ec) {
        return std::nullopt;
    }
    return WarmStamp{size,      static_cast<s64>(time.time_since_epoch().count()),
                     entries,   driver,
                     device_id, cache_version};
}

std::optional<WarmStamp> ReadWarmMarker(const fs::path& warm_file) {
    std::ifstream in(WarmMarkerPath(warm_file));
    WarmStamp stamp;
    if (!(in >> stamp.size >> stamp.write_time >> stamp.entries >> stamp.driver >>
          stamp.device_id >> stamp.cache_version)) {
        return std::nullopt;
    }
    return stamp;
}

bool WriteWarmMarker(const fs::path& warm_file, const WarmStamp& stamp) {
    std::ofstream out(WarmMarkerPath(warm_file), std::ios::trunc);
    out << stamp.size << ' ' << stamp.write_time << ' ' << stamp.entries << ' ' << stamp.driver
        << ' ' << stamp.device_id << ' ' << stamp.cache_version << '\n';
    out.close();
    return static_cast<bool>(out);
}

bool HasWarmFile(u64 title_id) {
    std::error_code ec;
    return fs::exists(ComputeWarmPath(title_id), ec);
}

bool DeleteWarmFiles(u64 title_id, std::string* error) {
    const auto warm = ComputeWarmPath(title_id);
    fs::path tmp = warm;
    tmp += ".tmp";
    bool ok = true;
    for (const auto& path :
         {warm, WarmMarkerPath(warm), WarmCountPath(warm), tmp, GraphicsWarmPath(title_id),
          PairsWarmPath(title_id), WarmMarkerPath(PairsWarmPath(title_id)),
          WarmCountPath(PairsWarmPath(title_id)), PairsProgressPath(title_id)}) {
        std::error_code ec;
        fs::remove(path, ec);
        if (ec) {
            ok = false;
            if (error != nullptr) {
                *error = ec.message();
            }
        }
    }
    return ok;
}

std::optional<std::string> WarmFilesStale(const fs::path& warm_file) {
    std::error_code ec;
    if (!fs::exists(warm_file, ec)) {
        return "none built yet";
    }
    {
        std::ifstream in(warm_file, std::ios::binary);
        std::array<char, 8> magic{};
        u32 version{};
        in.read(magic.data(), magic.size()).read(reinterpret_cast<char*>(&version), sizeof(version));
        if (!in || version != WarmCacheVersion) {
            return "built for another cache version";
        }
    }
    std::ifstream in(WarmCountPath(warm_file));
    u64 entries{};
    u64 size{};
    u32 builder{};
    if (!(in >> entries >> size >> builder) || builder != WarmBuilderVersion) {
        return "built by an older version";
    }
    if (fs::file_size(warm_file, ec) != size || ec) {
        return "changed since it was built";
    }
    return std::nullopt;
}

bool RefreshWarmFiles(Core::System& system, u64 title_id, std::stop_token stop) {
    if (title_id == 0) {
        return false;
    }
    const fs::path cache = TitleDir(title_id) / "vulkan.bin";
    const fs::path plan = PlanPath(title_id);
    const fs::path filled = GraphicsWarmPath(title_id);
    const bool verify = EnvOn("EDEN_NX2_PLAN_VERIFY");
    if (verify || EnvOn("EDEN_NX2_PLAN_EXPORT")) {
        if (const auto records = ExportPlan(cache, plan)) {
            LOG_INFO(Render_Vulkan, "Pipeline list for {:016x}: {} records exported", title_id,
                     *records);
        } else {
            LOG_WARNING(Render_Vulkan, "Pipeline list for {:016x}: no pipeline cache to export",
                        title_id);
        }
    }
    // Switch 1 titles have no compute precache: only a pipeline list or shader list scans them.
    auto reason = Settings::CurrentProgramIsSwitch2() ? WarmFilesStale(ComputeWarmPath(title_id))
                                                      : std::nullopt;
    std::error_code plan_ec;
    if (!reason && fs::exists(plan, plan_ec) &&
        (verify || !fs::exists(filled, plan_ec) ||
         fs::last_write_time(filled, plan_ec) < fs::last_write_time(plan, plan_ec))) {
        // ponytail: an unusable list (other version) is scanned for at every boot; check its
        // header here if lists outlive cache versions in the field.
        reason = "its pipeline list is new";
    }
    if (!reason && !fs::exists(plan, plan_ec) && fs::exists(PeanutsListPath(title_id), plan_ec) &&
        WarmFilesStale(PairsWarmPath(title_id))) {
        reason = "its shader list is new";
    }
    if (!reason) {
        return false;
    }
    const std::string& file_path = system.GetCurrentApplicationFilePath();
    if (file_path.empty()) {
        return false;
    }
    fs::path game_path = Common::FS::ToU8String(file_path);
    // Loose dumps run from their exefs folder; the loader opens its main.
    if (std::error_code ec; fs::is_directory(game_path, ec)) {
        game_path /= "main";
    }
    LOG_INFO(Render_Vulkan, "Shader precache for {:016x}: {}, building it", title_id, *reason);
    const bool ok = BuildWarmFiles(system, game_path, title_id, {}, std::move(stop)).ok;
    if (verify) {
        const PlanCheck check = CheckFilledRecords(cache, filled);
        LOG_INFO(Render_Vulkan,
                 "Pipeline list check for {:016x}: {} matched, {} mismatched, {} not found",
                 title_id, check.matched, check.mismatched, check.not_found);
    }
    return ok;
}

Result BuildWarmFiles(Core::System& system, const fs::path& game_path, u64 title_id,
                      const std::function<void(const Progress&)>& progress,
                      std::stop_token stop) {
    return BuildWarmFilesWith(system, game_path, title_id, BuildOptions{}, progress,
                              std::move(stop));
}

Result BuildWarmFilesWith(Core::System& system, const fs::path& game_path, u64 title_id,
                          const BuildOptions& options,
                          const std::function<void(const Progress&)>& progress,
                          std::stop_token stop) {
    Result result;
    u64 total_bytes = 0;
    u64 bytes_done = 0;
    size_t seq = 0;
    size_t consumed = 0;
    double reported_fraction = 0.0;
    auto last_report = std::chrono::steady_clock::time_point{};
    const auto report = [&](std::string_view stage, size_t done, size_t total) {
        if (!progress) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const bool last = total != 0 && done == total;
        if (!last && now - last_report < std::chrono::milliseconds{50}) {
            return;
        }
        last_report = now;
        const double extracted =
            total_bytes != 0 ? static_cast<double>(bytes_done) / static_cast<double>(total_bytes)
                             : 0.0;
        double fraction = ExtractionShare * extracted;
        if (stage == StageConverting) {
            fraction = ExtractionShare + (1.0 - ExtractionShare) *
                                             (seq != 0 ? static_cast<double>(consumed) /
                                                             static_cast<double>(seq)
                                                       : 1.0);
        } else if (stage == StageWriting) {
            fraction = last ? 1.0 : reported_fraction;
        }
        reported_fraction = std::max(reported_fraction, std::min(fraction, 1.0));
        progress(Progress{std::string(stage), done, total, reported_fraction, seq});
    };
    const auto begin = std::chrono::steady_clock::now();
    report(StageReading, 0, 0);

    const fs::path output_dir = options.output_dir.empty() ? TitleDir(title_id) : options.output_dir;
    std::error_code ec;
    fs::create_directories(output_dir, ec);
    const fs::path compute_path = output_dir / "warm-compute.bin";
    fs::path compute_tmp = compute_path;
    compute_tmp += ".tmp";
    std::ofstream compute_out;
    const auto fail = [&](std::string error) {
        compute_out.close();
        std::error_code ignored;
        fs::remove(compute_tmp, ignored);
        result.ok = false;
        result.error = std::move(error);
        LOG_WARNING(Render_Vulkan, "Shader precache build for {:016x} failed: {}", title_id,
                    result.error);
        return result;
    };

    GameFiles game;
    if (const auto error = OpenGame(system, game_path, game, stop)) {
        return fail(*error);
    }
    if (stop.stop_requested()) {
        return fail("Stopped");
    }

    // Archive list, in the offline pipeline's order: every shader archive of each package, then
    // each package's effects, sorted by path inside a package (see ClassifyGameFile).
    std::vector<ArchiveFile> archives;
    std::vector<ArchiveFile> effects;
    ZstdDictionaries dictionaries;
    FileSys::VirtualFile dictionary_pack;
    for (const auto& romfs : game.romfs) {
        std::vector<ArchiveFile> files;
        ListFiles(romfs, "", files);
        std::sort(files.begin(), files.end(),
                  [](const ArchiveFile& a, const ArchiveFile& b) { return a.path < b.path; });
        std::vector<ArchiveFile> package_effects;
        for (auto& file : files) {
            switch (ClassifyGameFile(file.path)) {
            case GameFileKind::ShaderArchive:
                archives.push_back(file);
                break;
            case GameFileKind::Effect:
                package_effects.push_back(file);
                break;
            case GameFileKind::ZstdDictionaries:
                dictionary_pack = file.file; // the newest package wins
                break;
            case GameFileKind::Other:
                break;
            }
        }
        effects.insert(effects.end(), package_effects.begin(), package_effects.end());
    }
    archives.insert(archives.end(), effects.begin(), effects.end());
    for (const auto& archive : archives) {
        total_bytes += archive.file->GetSize();
    }
    if (stop.stop_requested()) {
        return fail("Stopped");
    }
    if (dictionary_pack) {
        try {
            dictionaries.Load(dictionary_pack->ReadAllBytes());
        } catch (const ArchiveError& e) {
            LOG_WARNING(Render_Vulkan, "Shader precache: zstd dictionary pack: {}", e.what());
        }
    }
    // Without archives the file is written empty: the precache then has only the pipeline cache
    // of earlier sessions to build.
    result.archives = archives.size();
    LOG_INFO(Render_Vulkan, "Shader precache build: {} archives, {} zstd dictionaries",
             archives.size(), dictionaries.Count());

    compute_out.open(compute_tmp, std::ios::binary | std::ios::trunc);
    if (!compute_out) {
        return fail("Cannot write to " + Common::FS::PathToUTF8String(output_dir));
    }
    WriteHeader(compute_out);

    // The title's pipeline list: the scan keeps the lowered code of the programs it names.
    const std::vector<u8> plan_bytes = ReadFile(output_dir / "plan.bin");
    const auto plan = ReadPlan(plan_bytes);
    std::unordered_map<u64, std::vector<u8>> plan_code;
    if (plan) {
        for (const PlanRecord& record : *plan) {
            for (const PlanEnv& env : record.envs) {
                plan_code.try_emplace(env.hash);
            }
        }
    } else if (!plan_bytes.empty()) {
        LOG_WARNING(Render_Vulkan, "Shader precache: plan.bin is not a pipeline list for this "
                                   "version, ignored");
    }
    std::unordered_set<PairHash, PairHasher> graphics_identities;
    // Without a pipeline list, Peanuts Native's list of the title's shaders.
    std::optional<PeanutsList> list;
    if (plan_bytes.empty()) {
        if (const auto bytes = ReadFile(output_dir / "peanuts.tsv.zst"); !bytes.empty()) {
            list = ReadPeanutsList(bytes);
        }
    }
    /// The list's shaders found in the game files: their stage and program number (0: none).
    std::unordered_map<u64, std::pair<ProgramStage, u32>> list_found;
    // Every vertex and fragment program of a Switch 2 title (once per code), a Peanuts list's
    // Switch 1 programs, and the pairs the BNSH variations make of them.
    std::vector<PairProgram> programs;
    std::unordered_map<PairHash, u32, PairHasher> program_numbers;
    std::vector<std::pair<u32, u32>> file_pairs;
    std::unordered_set<u64> file_pair_set;

    std::unordered_set<PairHash, PairHasher> compute_identities;
    std::unordered_set<std::string> compute_keys;
    size_t archive_errors = 0;
    size_t member_errors = 0;

    const size_t threads =
        options.threads != 0
            ? options.threads
            : std::clamp<size_t>(std::thread::hardware_concurrency() / 4, 2, 6);
    auto converter = std::make_unique<Converter>(threads, stop);
    u64 other_stage_records = 0;
    u64 maxwell_records = 0;
    u64 maxwell_open = 0; ///< Switch 1 programs that do not end inside their code area
    u64 maxwell_padded = 0; ///< List programs given a zero last word (MaxwellProgram::padded)

    // Consumes finished conversions in submission order (first program wins on equal keys).
    const auto consume = [&](JobResult&& r) {
        ++consumed;
        if (r.ok) {
            if (const auto it = plan_code.find(r.hash);
                it != plan_code.end() && it->second.empty()) {
                it->second = std::move(r.code);
            }
        }
        if (r.graphics) {
            if (r.ok && r.pair_program != 0) {
                PairProgram& program = programs[r.pair_program - 1];
                program.hash = r.hash;
                program.entry = std::move(r.entry);
            }
            return;
        }
        if (!r.ok) {
            ++result.skipped;
            return;
        }
        const std::string key(reinterpret_cast<const char*>(r.compute_key.data()),
                              r.compute_key.size());
        if (compute_keys.insert(key).second) {
            compute_out.write(reinterpret_cast<const char*>(r.bytes.data()),
                              static_cast<std::streamsize>(r.bytes.size()));
            ++result.compute_programs;
        }
    };
    const auto drain = [&] {
        while (auto r = converter->Next(false)) {
            consume(std::move(*r));
        }
    };

    for (size_t archive_index = 0; archive_index < archives.size(); ++archive_index) {
        if (stop.stop_requested()) {
            converter.reset();
            return fail("Stopped");
        }
        report(StageExtracting, archive_index, archives.size());
        const auto& archive = archives[archive_index];
        const auto data = archive.file->ReadAllBytes();
        // Half of an archive's share when read, the rest when walked.
        const u64 archive_bytes = archive.file->GetSize();
        bytes_done += archive_bytes / 2;
        report(StageExtracting, archive_index, archives.size());
        const auto name = archive.path.substr(archive.path.rfind('/') + 1);
        // The program (ShaderRecord::group) being read and its stages so far.
        u32 group = 0;
        u32 group_vertex = 0;
        u32 group_fragment = 0;
        const auto on_record = [&](const ShaderRecord& record) {
            if (record.maxwell) {
                // Switch 1 programs are not converted: the pipeline list's get their code as the
                // runtime caches it. Which end the runtime took depends on the guest driver the
                // game set up (texture_bound), so both are offered.
                ++maxwell_records;
                const bool headerless = record.stage == ProgramStage::Compute &&
                                        (record.code.size() < 4 || record.code[0] != 0x79 ||
                                         record.code[1] != 0x56 || record.code[2] != 0x34 ||
                                         record.code[3] != 0x12);
                const size_t start =
                    record.stage != ProgramStage::Compute ? 0x30 : headerless ? 0 : 0x100;
                const auto list_key = list ? PeanutsKey(record.control, record.code) : std::nullopt;
                auto* const listed =
                    list_key && list->shaders.contains(*list_key) && !list_found.contains(*list_key)
                        ? &list_found.try_emplace(*list_key, record.stage, 0U).first->second
                        : nullptr;
                for (const bool proprietary : {true, false}) {
                    auto program = FindMaxwellProgram(record.code, start, proprietary);
                    if (!program) {
                        maxwell_open += proprietary ? 1 : 0;
                        continue;
                    }
                    if (listed != nullptr && proprietary && program->code.size() >= 0x50 &&
                        (record.stage == ProgramStage::Vertex ||
                         record.stage == ProgramStage::Fragment)) {
                        // Guest driver 2 (texture_bound), which Switch 1 titles normally use.
                        programs.push_back({record.stage, program->hash,
                                            PairProgramEntry({program->code, 2}, record.stage)});
                        listed->second = static_cast<u32>(programs.size());
                    }
                    if (const auto it = plan_code.find(program->hash);
                        it != plan_code.end() && it->second.empty()) {
                        maxwell_padded += program->padded ? 1 : 0;
                        it->second = std::move(program->code);
                    }
                }
                return;
            }
            Job job;
            // The first record with a key the Peanuts list names is that shader.
            const auto list_key = list ? PeanutsKey(record.control, record.code) : std::nullopt;
            const bool listed = list_key && list->shaders.contains(*list_key) &&
                                list_found.try_emplace(*list_key, record.stage, 0U).second;
            if (record.stage != ProgramStage::Compute) {
                ++other_stage_records;
                if (record.stage == ProgramStage::Vertex ||
                    record.stage == ProgramStage::Fragment) {
                    const auto [it, added] = program_numbers.try_emplace(
                        PairHash{Hash128(record.code), {}}, static_cast<u32>(programs.size() + 1));
                    const u32 number = it->second;
                    if (listed) {
                        list_found[*list_key].second = number;
                    }
                    if (record.group != 0) {
                        if (record.group != group) {
                            group = record.group;
                            group_vertex = group_fragment = 0;
                        }
                        (record.stage == ProgramStage::Vertex ? group_vertex : group_fragment) =
                            number;
                        if (group_vertex != 0 && group_fragment != 0 &&
                            file_pair_set.insert(u64{group_vertex} << 32 | group_fragment).second) {
                            file_pairs.emplace_back(group_vertex, group_fragment);
                        }
                    }
                    if (!added) {
                        return;
                    }
                    programs.push_back({record.stage});
                    job.pair_program = number;
                    job.fragment = record.stage == ProgramStage::Fragment;
                } else if (plan_code.empty() ||
                           !graphics_identities.insert(PairHash{Hash128(record.code), {}}).second) {
                    // The others are lowered only to find the code of the pipeline list's programs.
                    return;
                }
                job.graphics = true;
                job.geometry = record.stage == ProgramStage::Geometry;
            } else {
                if (!compute_identities
                         .insert(PairHash{Hash128(record.code), Hash128(record.control)})
                         .second) {
                    return;
                }
                const auto values = ReadComputeControl(record.control);
                if (!values || !values->workgroup[0] || !values->workgroup[1] ||
                    !values->workgroup[2]) {
                    ++result.skipped;
                    return;
                }
                job.control = *values;
            }
            job.seq = seq++;
            job.program.assign(record.code.begin(), record.code.end());
            converter->Push(std::move(job));
            while (converter->Pending() > MaxPendingJobs && !stop.stop_requested()) {
                if (auto r = converter->Next(true)) {
                    consume(std::move(*r));
                }
            }
        };
        const auto on_member_error = [&](const std::string& error) {
            ++member_errors;
            LOG_DEBUG(Render_Vulkan, "Shader precache: {}: {}", archive.path, error);
        };
        try {
            WalkArchive(data, name, &dictionaries, on_record, on_member_error, stop);
        } catch (const ArchiveError& e) {
            ++archive_errors;
            LOG_WARNING(Render_Vulkan, "Shader precache: {}: {}", archive.path, e.what());
        }
        bytes_done += archive_bytes - archive_bytes / 2;
        drain();
    }
    report(StageExtracting, archives.size(), archives.size());

    const size_t total_jobs = seq;
    while (consumed < total_jobs) {
        if (stop.stop_requested()) {
            converter.reset();
            return fail("Stopped");
        }
        report(StageConverting, consumed, total_jobs);
        if (auto r = converter->Next(true)) {
            consume(std::move(*r));
        }
    }
    report(StageConverting, total_jobs, total_jobs);
    converter.reset();

    report(StageWriting, 0, 1);
    compute_out.close();
    if (!compute_out) {
        return fail("Writing the precache file failed");
    }
    // A new file: the marker and count of the old one no longer apply.
    std::error_code ignored;
    fs::remove(WarmMarkerPath(compute_path), ignored);
    fs::remove(WarmCountPath(compute_path), ignored);
    fs::rename(compute_tmp, compute_path, ec);
    if (ec) {
        return fail("Cannot replace " + Common::FS::PathToUTF8String(compute_path));
    }
    WriteWarmCount(compute_path, result.compute_programs);
    if (plan) {
        // Every list record whose programs were all found, as a pipeline cache record.
        size_t filled = 0;
        std::unordered_set<u64> missing;
        WriteCacheFile(output_dir / "warm-graphics.bin", [&](std::ofstream& out) {
            for (const PlanRecord& record : *plan) {
                bool found = true;
                for (const PlanEnv& env : record.envs) {
                    const auto& code = plan_code.at(env.hash);
                    if (code.empty() || code.size() != env.CodeSize()) {
                        missing.insert(env.hash);
                        found = false;
                    }
                }
                if (!found) {
                    continue;
                }
                const std::vector<u8> bytes = FilledRecord(record, plan_code);
                out.write(reinterpret_cast<const char*>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size()));
                ++filled;
            }
        });
        LOG_INFO(Render_Vulkan,
                 "Pipeline list for {:016x}: {} records, {} filled, {} missing ({} programs not "
                 "found in the game files); {} programs filled with a zero last word",
                 title_id, plan->size(), filled, plan->size() - filled, missing.size(),
                 maxwell_padded);
    }
    if (list || !file_pairs.empty()) {
        // The programs that lowered, each once, then the pairs: the game files' own and the
        // Peanuts list's. Which of them are built, and with what, is decided at boot.
        std::vector<u32> indices(programs.size(), ~0U);
        u32 program_count = 0;
        for (size_t i = 0; i < programs.size(); ++i) {
            const bool paired = programs[i].stage == ProgramStage::Vertex ||
                                programs[i].stage == ProgramStage::Fragment;
            if (paired && !programs[i].entry.empty()) {
                indices[i] = program_count++;
            }
        }
        std::vector<std::pair<u32, u32>> pairs;
        std::set<std::pair<u32, u32>> pair_set;
        const auto add = [&](u32 vertex, u32 fragment) {
            if (vertex == 0 || fragment == 0 || indices[vertex - 1] == ~0U ||
                indices[fragment - 1] == ~0U ||
                programs[vertex - 1].stage != ProgramStage::Vertex ||
                programs[fragment - 1].stage != ProgramStage::Fragment) {
                return;
            }
            const std::pair<u32, u32> pair{indices[vertex - 1], indices[fragment - 1]};
            if (pair_set.insert(pair).second) {
                pairs.push_back(pair);
            }
        };
        for (const auto& [vertex, fragment] : file_pairs) {
            add(vertex, fragment);
        }
        size_t list_pairs_found = 0;
        if (list) {
            for (const auto& [vertex_key, fragment_key] : list->pairs) {
                const auto vertex = list_found.find(vertex_key);
                const auto fragment = list_found.find(fragment_key);
                if (vertex != list_found.end() && fragment != list_found.end()) {
                    ++list_pairs_found;
                    add(vertex->second.second, fragment->second.second);
                }
            }
        }
        const fs::path pairs_path = output_dir / "warm-pairs.bin";
        fs::remove(WarmMarkerPath(pairs_path), ignored);
        WriteCacheFile(pairs_path, [&](std::ofstream& out) {
            Write(out, PairsTag);
            Write(out, program_count);
            for (size_t i = 0; i < programs.size(); ++i) {
                if (indices[i] == ~0U) {
                    continue;
                }
                Write(out, programs[i].hash);
                Write(out, static_cast<u8>(programs[i].stage == ProgramStage::Fragment));
                out.write(reinterpret_cast<const char*>(programs[i].entry.data()),
                          static_cast<std::streamsize>(programs[i].entry.size()));
            }
            Write(out, static_cast<u32>(pairs.size()));
            out.write(reinterpret_cast<const char*>(pairs.data()),
                      static_cast<std::streamsize>(pairs.size() * sizeof(pairs[0])));
        });
        WriteWarmCount(pairs_path, pairs.size());
        LOG_INFO(Render_Vulkan,
                 "Shader pairs for {:016x}: {} vertex and fragment programs, {} pairs in the game "
                 "files' BNSH variations, {} pairs written",
                 title_id, program_count, file_pairs.size(), pairs.size());
        if (list) {
            size_t found = 0;
            for (const u64 key : list->shaders) {
                found += list_found.contains(key) ? 1 : 0;
            }
            LOG_INFO(Render_Vulkan,
                     "Peanuts list for {:016x}: {} of {} shaders found in the game files ({} not "
                     "found), {} of {} pairs with both shaders found",
                     title_id, found, list->shaders.size(), list->shaders.size() - found,
                     list_pairs_found, list->pairs.size());
        }
    }
    report(StageWriting, 1, 1);
    result.ok = true;
    LOG_INFO(Render_Vulkan,
             "Shader precache build: {} records of other stages (not precached), {} Switch 1 "
             "programs ({} without an end in their code area)",
             other_stage_records, maxwell_records, maxwell_open);
    LOG_INFO(Render_Vulkan,
             "Shader precache build for {:016x}: {} compute entries, {} programs skipped, {} "
             "archive and {} member errors, {:.1f} s",
             title_id, result.compute_programs, result.skipped, archive_errors, member_errors,
             std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count());
    return result;
}

} // namespace VideoCommon::ShaderWarm
