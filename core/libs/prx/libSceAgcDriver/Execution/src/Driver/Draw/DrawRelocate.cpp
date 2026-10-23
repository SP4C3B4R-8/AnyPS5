#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace AgcDriver::DriverDetail {

namespace {

constexpr std::size_t MaxAliases = 16384;
constexpr std::size_t MaxContentWords = 64;

std::uint64_t AlignedWord(std::uint32_t word) {
    return word & ~std::uint32_t {3};
}

enum RebaseRefusal : std::size_t { RebaseVertexInfo, RebaseVertexBase, RebaseVertexFormat, RebaseBaseWordVertex, RebaseVertexLayout, RebaseDecode, RebaseUserData, RebaseHighWord, RebaseBaseWord, RebaseZero, RebaseAmbiguous, RebaseDescriptor, RebaseUnhandled, RebaseTables, RebaseRefusalCount };

constexpr std::array<const char*, RebaseRefusalCount> RebaseRefusalNames {"vertex layout changed", "vertex buffer stride or alignment changed", "vertex buffer format changed", "walked pointer moved in a vertex stage", "vertex runs not rebuildable", "vertex decode failed", "no captured user data", "word past 64", "walked pointer moved", "zero word", "ambiguous value", "descriptor field changed", "value not found in bindings", "image tables"};

struct RebaseCounters {
    std::array<std::atomic<std::uint64_t>, RebaseRefusalCount> refused {};
    std::atomic<std::uint64_t> tried {0}, built {0}, equal {0}, differing {0};
};

RebaseCounters& Rebases() {
    static RebaseCounters counters;
    return counters;
}

void ReportRebases() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    auto& counters = Rebases();
    if (!profile || counters.tried.load(std::memory_order_relaxed) < 100000) return;
    char refusals[512];
    std::size_t used = 0;
    for (std::size_t reason = 0; reason < RebaseRefusalCount && used < sizeof(refusals); ++reason) {
        const auto count = counters.refused[reason].exchange(0, std::memory_order_relaxed);
        if (count != 0) used += static_cast<std::size_t>(std::snprintf(refusals + used, sizeof(refusals) - used, "%s %s %llu", used == 0 ? "" : ",", RebaseRefusalNames[reason], static_cast<unsigned long long>(count)));
    }
    refusals[std::min(used, sizeof(refusals) - 1)] = 0;
    AgcDriver::ProfilePrint_nid_no_patch("[draw-rebase] %llu data-only stages differing in used user words: %llu rebased (%llu validated equal, %llu not); refused:%s\n", static_cast<unsigned long long>(counters.tried.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(counters.built.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(counters.equal.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(counters.differing.exchange(0, std::memory_order_relaxed)), refusals);
}

}

bool Driver::drawRelocationEnabled() {
    static const bool enabled = std::getenv("APS5_NO_DRAW_RELOCATION") == nullptr;
    return enabled;
}

std::uint64_t Driver::relocatedContentKey(const RelocatableUserWord& word, std::uint32_t value, bool& readable) {
    std::uint64_t hash = 0x9e3779b97f4a7c15ull;
    const auto mix = [&](std::uint64_t mixed) {
        hash ^= mixed;
        hash *= 0x100000001b3ull;
    };
    mix(value & 3u);
    if (value == 0) {
        readable = false;
        return hash;
    }
    std::array<std::uint32_t, MaxContentWords> content;
    for (const auto& [offset, words] : word.reads) {
        mix(static_cast<std::uint64_t>(offset));
        mix(words);
        for (std::size_t done = 0; done < words; done += MaxContentWords) {
            const auto count = std::min<std::size_t>(words - done, MaxContentWords);
            const auto address = AlignedWord(value) + static_cast<std::uint64_t>(offset) + done * sizeof(std::uint32_t);
            if (GuestMemory::CopyMapped(address, std::as_writable_bytes(std::span(content).first(count))) != GuestMemory::Compare::Equal) {
                readable = false;
                return hash;
            }
            for (std::size_t i = 0; i < count; ++i) mix(content[i]);
        }
    }
    return hash;
}

void Driver::tagVariantRuns(DispatchVariant& variant, std::span<const ShaderRecompiler::MemoryRegion> regions, const ShaderRecompiler::SrtUserDataUse& use, bool eligible, std::uint32_t hiddenWords, std::span<const std::shared_ptr<const ShaderRecompiler::SrtGuestWords>> supplied) {
    std::uint64_t relocatable = eligible && drawRelocationEnabled() ? use.asBase & ~use.asValue : 0;
    if (hiddenWords != 0) relocatable &= hiddenWords >= 64 ? 0 : ~((std::uint64_t {1} << hiddenWords) - 1);
    std::vector<std::pair<std::uint64_t, std::int32_t>> reads;
    const auto covered = [&](std::uint64_t address) {
        return std::any_of(regions.begin(), regions.end(), [&](const ShaderRecompiler::MemoryRegion& region) { return address >= region.guestAddress && address - region.guestAddress + sizeof(std::uint32_t) <= region.bytes.size(); });
    };
    if (relocatable != 0) {
        reads = use.reads;
        std::sort(reads.begin(), reads.end());
        reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
        const auto drop = [&](std::int32_t origin) {
            if (origin >= 0) relocatable &= ~(std::uint64_t {1} << origin);
        };
        for (std::size_t i = 0; i < reads.size(); ++i) {
            if (i + 1 < reads.size() && reads[i].first == reads[i + 1].first) {
                drop(reads[i].second);
                drop(reads[i + 1].second);
            }
            if (reads[i].second >= 0 && ((reads[i].first & 3u) != 0 || !covered(reads[i].first))) drop(reads[i].second);
        }
    }
    const auto origin = [&](std::uint64_t address) -> std::int8_t {
        const auto found = std::lower_bound(reads.begin(), reads.end(), std::make_pair(address, std::numeric_limits<std::int32_t>::min()));
        if (found == reads.end() || found->first != address || found->second < 0) return -1;
        return (relocatable >> found->second & 1u) != 0 ? static_cast<std::int8_t>(found->second) : std::int8_t {-1};
    };
    variant.relocatableWords = relocatable;
    variant.valueWords = use.asValue;
    variant.baseWords = use.asBase;
    for (std::size_t index = 0; index < regions.size(); ++index) {
        const auto& region = regions[index];
        const auto count = region.bytes.size() / sizeof(std::uint32_t);
        const auto offset = variant.words.size();
        variant.words.resize(offset + count);
        std::memcpy(variant.words.data() + offset, region.bytes.data(), count * sizeof(std::uint32_t));
        if (relocatable == 0) {
            AppendSharedWords(variant.shared, variant.runs.size(), offset, region.bytes, supplied);
            variant.runs.emplace_back(region.guestAddress, region.guestAddress + region.bytes.size());
            continue;
        }
        std::size_t start = 0;
        auto current = count != 0 ? origin(region.guestAddress) : std::int8_t {-1};
        for (std::size_t word = 1; word <= count; ++word) {
            const auto next = word < count ? origin(region.guestAddress + word * sizeof(std::uint32_t)) : std::int8_t {-2};
            if (next == current) continue;
            AppendSharedWords(variant.shared, variant.runs.size(), offset + start, region.bytes.subspan(start * sizeof(std::uint32_t), (word - start) * sizeof(std::uint32_t)), supplied);
            variant.runs.emplace_back(region.guestAddress + start * sizeof(std::uint32_t), region.guestAddress + word * sizeof(std::uint32_t));
            variant.runBases.push_back(current);
            start = word;
            current = next;
        }
    }
}

namespace {

template <typename TMap>
std::size_t dropEveryOther(TMap& map) {
    std::size_t dropped = 0;
    bool drop = false;
    for (auto it = map.begin(); it != map.end();) {
        drop = !drop;
        if (!drop) {
            ++it;
            continue;
        }
        it = map.erase(it);
        ++dropped;
    }
    return dropped;
}

}

void Driver::learnPendingRelocation() {
    const auto pending = drawRelocationPending;
    drawRelocationPending = {};
    if (pending.key == 0) return;
    const auto inserted = drawCache.find(pending.key);
    if (inserted == drawCache.end() || inserted->second->decode == nullptr) return;
    const auto& entry = *inserted->second;
    const auto& programs = entry.decode->programs;
    if (entry.stages.size() != programs.size()) return;
    const auto known = drawRelocations.find(pending.identity);
    auto shape = std::make_shared<DrawRelocation>();
    if (known != drawRelocations.end()) *shape = *known->second;
    bool changed = known == drawRelocations.end();
    for (std::size_t i = 0; i < programs.size(); ++i) {
        if (entry.stages[i].empty()) continue;
        const auto& variant = *entry.stages[i].front();
        const auto& program = programs[i];
        for (std::uint32_t index = program.hiddenUserWords; index < program.userData.size() && index < 64; ++index) {
            const auto reg = program.userDataBase + index - program.hiddenUserWords;
            const auto held = std::find_if(shape->words.begin(), shape->words.end(), [&](const RelocatableUserWord& word) { return word.reg == reg; });
            if ((variant.relocatableWords >> index & 1u) == 0) {
                if (held == shape->words.end()) continue;
                shape->words.erase(held);
                shape->banned.push_back(reg);
                changed = true;
                continue;
            }
            if (held != shape->words.end() || std::find(shape->banned.begin(), shape->banned.end(), reg) != shape->banned.end()) continue;
            RelocatableUserWord word{reg, {}};
            const auto base = AlignedWord(program.userData[index]);
            for (std::size_t run = 0; run < variant.runs.size() && run < variant.runBases.size(); ++run) {
                if (variant.runBases[run] != static_cast<std::int8_t>(index)) continue;
                word.reads.emplace_back(static_cast<std::int64_t>(variant.runs[run].first - base), static_cast<std::uint32_t>((variant.runs[run].second - variant.runs[run].first) / sizeof(std::uint32_t)));
            }
            shape->words.push_back(std::move(word));
            changed = true;
        }
    }
    if (changed) {
        std::sort(shape->words.begin(), shape->words.end(), [](const RelocatableUserWord& a, const RelocatableUserWord& b) { return a.reg < b.reg; });
        drawRelocations[pending.identity] = std::move(shape);
    }
    if (pending.alias == 0) return;
    if (drawAliases.size() >= MaxAliases) drawRelocationCounters.evicted += dropEveryOther(drawAliases);
    drawAliases[pending.alias] = inserted->second;
}

void Driver::relocateDrawEntry(const QueueState& queue, std::uint64_t alias, std::shared_ptr<DrawEntry>& entry, std::shared_ptr<const DrawDecode>& decode, std::vector<DrawUserPatch>& patches) {
    auto& counters = drawRelocationCounters;
    ++counters.lookups;
    const auto found = drawAliases.find(alias);
    if (found == drawAliases.end() || found->second->decode == nullptr) return;
    const auto donor = found->second;
    const auto& programs = donor->decode->programs;
    if (donor->stages.size() != programs.size()) return;
    struct Move {
        std::size_t program;
        std::uint32_t index, from, to;
    };
    std::vector<Move> moves;
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto& program = programs[i];
        for (std::uint32_t index = program.hiddenUserWords; index < program.userData.size(); ++index) {
            const auto current = readUserData(queue.shader, program.userDataBase + index - program.hiddenUserWords);
            if (current != program.userData[index]) moves.push_back({i, index, program.userData[index], current});
        }
    }
    if (moves.empty()) return;
    ++counters.donors;
    auto relocated = std::make_shared<DrawEntry>();
    relocated->decode = donor->decode;
    relocated->stages.resize(donor->stages.size());
    for (std::size_t i = 0; i < donor->stages.size(); ++i) {
        const bool moved = std::any_of(moves.begin(), moves.end(), [&](const Move& move) { return move.program == i; });
        if (!moved) {
            relocated->stages[i] = donor->stages[i];
            continue;
        }
        for (const auto& variant : donor->stages[i]) {
            if (variant == nullptr || variant->compiled == nullptr) continue;
            const bool allowed = std::all_of(moves.begin(), moves.end(), [&](const Move& move) { return move.program != i || (move.index < 64 && (variant->relocatableWords >> move.index & 1u) != 0 && move.from != 0 && move.to != 0 && ((move.from ^ move.to) & 3u) == 0); });
            if (!allowed) {
                ++counters.refused;
                continue;
            }
            auto copy = std::make_shared<DispatchVariant>();
            copy->runs = variant->runs;
            copy->runBases = variant->runBases;
            copy->relocatableWords = variant->relocatableWords;
            copy->words = variant->words;
            copy->forgetSerial = variant->forgetSerial;
            copy->compiled = variant->compiled;
            copy->meshIndexDescriptor = variant->meshIndexDescriptor;
            copy->shader = variant->shader;
            copy->dataPositions = variant->dataPositions;
            copy->dataSlots = variant->dataSlots;
            copy->flatBinding = variant->flatBinding;
            copy->pushOffset = variant->pushOffset;
            copy->vertexInfo = variant->vertexInfo;
            for (const auto& move : moves) {
                if (move.program != i) continue;
                const auto delta = AlignedWord(move.to) - AlignedWord(move.from);
                for (std::size_t run = 0; run < copy->runs.size() && run < copy->runBases.size(); ++run) {
                    if (copy->runBases[run] != static_cast<std::int8_t>(move.index)) continue;
                    copy->runs[run].first += delta;
                    copy->runs[run].second += delta;
                }
                const auto& forwarded = variant->compiled->shaderDataUserWords;
                for (std::uint32_t slot = 0; slot < forwarded.size(); ++slot) {
                    if (forwarded[slot] == move.index) copy->shaderDataPatches.emplace_back(slot, move.to);
                }
            }
            relocated->stages[i].push_back(std::move(copy));
        }
    }
    patches.clear();
    for (const auto& move : moves) patches.push_back({move.program, move.index, move.to});
    entry = std::move(relocated);
    decode = donor->decode;
}

bool Driver::drawRebaseEnabled() {
    static const bool enabled = std::getenv("APS5_NO_DRAW_REBASE") == nullptr;
    return enabled;
}

void Driver::noteDataRebase(bool equal) {
    auto& counters = Rebases();
    (equal ? counters.equal : counters.differing).fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<DispatchVariant> Driver::rebaseDataVariant(const DispatchVariant& variant, const DrawProgram& program, DrawRebaseDecode& decoded) {
    if (variant.compiled == nullptr) return nullptr;
    const std::span<const std::uint32_t> now = program.userData;
    auto& counters = Rebases();
    counters.tried.fetch_add(1, std::memory_order_relaxed);
    ReportRebases();
    const auto refuse = [&](RebaseRefusal reason) -> std::shared_ptr<DispatchVariant> {
        counters.refused[reason].fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    };
    const auto& was = variant.userData;
    if (was.size() != now.size()) return refuse(RebaseUserData);
    const auto& compiled = *variant.compiled;
    if (!compiled.imageTableRanges.empty() || (variant.vertexInfo == nullptr && !compiled.vertexAttributes.empty())) return refuse(RebaseTables);
    std::uint64_t fetchWords = 0;
    if (variant.vertexInfo != nullptr) {
        for (const auto reg : {variant.vertexInfo->fetchAttribReg, variant.vertexInfo->fetchBufferReg}) {
            if (reg + 1u >= 64u) return refuse(RebaseHighWord);
            fetchWords |= std::uint64_t {3} << reg;
        }
    }
    std::uint64_t moved = 0, relocated = 0;
    bool fetchMoved = false;
    for (std::size_t w = 0; w < now.size(); ++w) {
        if (now[w] == was[w]) continue;
        if (w >= 64u) return refuse(RebaseHighWord);
        if ((variant.userDataMask >> w & 1u) == 0) continue;
        if ((variant.relocatableWords >> w & 1u) != 0) {
            if (((now[w] ^ was[w]) & 3u) != 0 || was[w] == 0 || now[w] == 0) return refuse(RebaseBaseWord);
            relocated |= std::uint64_t {1} << w;
            moved |= std::uint64_t {1} << w;
            continue;
        }
        if ((variant.baseWords >> w & 1u) != 0) return refuse(variant.vertexInfo != nullptr ? RebaseBaseWordVertex : RebaseBaseWord);
        if ((fetchWords >> w & 1u) != 0 && (variant.valueWords >> w & 1u) == 0) {
            fetchMoved = true;
            continue;
        }
        if (was[w] == 0 || now[w] == 0) return refuse(RebaseZero);
        moved |= std::uint64_t {1} << w;
    }
    if (moved == 0 && !fetchMoved) return nullptr;
    if (variant.vertexInfo == nullptr && fetchMoved) return refuse(RebaseVertexInfo);
    for (auto left = moved; left != 0; left &= left - 1) {
        const auto a = static_cast<std::size_t>(std::countr_zero(left));
        for (auto right = left & (left - 1); right != 0; right &= right - 1) {
            const auto b = static_cast<std::size_t>(std::countr_zero(right));
            if (was[a] == was[b] && now[a] != now[b]) return refuse(RebaseAmbiguous);
        }
    }
    const auto movedFrom = [&](std::uint32_t value) -> int {
        for (auto left = moved; left != 0; left &= left - 1) {
            const auto w = std::countr_zero(left);
            if (was[static_cast<std::size_t>(w)] == value) return w;
        }
        return -1;
    };
    const auto& forwarded = compiled.shaderDataUserWords;
    const auto forwardedMoved = [&](std::size_t slot) { return slot < forwarded.size() && forwarded[slot] < 64u && (moved >> forwarded[slot] & 1u) != 0; };
    std::uint64_t handled = relocated;
    for (std::size_t slot = 0; slot < forwarded.size(); ++slot) {
        if (forwardedMoved(slot)) handled |= std::uint64_t {1} << forwarded[slot];
    }
    for (std::size_t at = 0; at + sizeof(std::uint32_t) <= compiled.pushConstants.size(); at += sizeof(std::uint32_t)) {
        if (forwardedMoved(at / sizeof(std::uint32_t))) continue;
        std::uint32_t value = 0;
        std::memcpy(&value, compiled.pushConstants.data() + at, sizeof(value));
        if (movedFrom(value) >= 0) return refuse(RebaseUnhandled);
    }
    struct DescriptorPatch {
        std::size_t binding, word;
        std::uint32_t value;
    };
    std::vector<DescriptorPatch> patches;
    for (std::size_t index = 0; index < compiled.bindings.size(); ++index) {
        const auto& binding = compiled.bindings[index];
        const auto& words = binding.guestDescriptor;
        if (binding.role == ShaderRecompiler::DescriptorRole::GuestBuffers) {
            if (words.size() % 4u != 0) return refuse(RebaseDescriptor);
            for (std::size_t d = 0; d < words.size(); ++d) {
                const auto w = movedFrom(words[d]);
                if (w < 0) continue;
                if ((relocated >> w & 1u) != 0) return refuse(RebaseUnhandled);
                const auto delta = now[static_cast<std::size_t>(w)] ^ was[static_cast<std::size_t>(w)];
                const auto field = d % 4u;
                if (!((field == 0 && (delta & 3u) == 0) || (field == 1 && (delta & 0xffff0000u) == 0))) return refuse(RebaseDescriptor);
                patches.push_back({index, d, now[static_cast<std::size_t>(w)]});
                handled |= std::uint64_t {1} << w;
            }
            continue;
        }
        for (std::size_t d = 0; d < words.size(); ++d) {
            if (binding.role == ShaderRecompiler::DescriptorRole::ShaderData && forwardedMoved(d)) continue;
            if (movedFrom(words[d]) >= 0) return refuse(RebaseUnhandled);
        }
    }
    if (handled != moved) return refuse(RebaseUnhandled);
    auto copy = std::make_shared<DispatchVariant>();
    if (variant.vertexInfo != nullptr) {
        if (!variant.shared.empty() || !variant.runBases.empty() || !variant.dataPositions.empty()) return refuse(RebaseVertexLayout);
        if (!decoded.tried) {
            decoded.tried = true;
            try {
                decoded.info = Graphics::DecodeVertexStageInfo(program.binary.header, program.binary.headerAddress, program.userData, &decoded.reads, false, 8u - program.firstUserSgpr);
            } catch (const std::exception&) {
                decoded.failed = true;
            }
        }
        if (decoded.failed) return refuse(RebaseDecode);
        const auto& old = *variant.vertexInfo;
        auto info = decoded.info;
        info.paClVsOutCntl = old.paClVsOutCntl;
        info.depthRemapMode = old.depthRemapMode;
        info.pointList = old.pointList;
        info.pointSize = old.pointSize;
        info.pointSizeMin = old.pointSizeMin;
        info.pointSizeMax = old.pointSizeMax;
        auto shape = info;
        for (std::uint32_t r = 0; r < shape.resourcesNum && r < shape.resources.size(); ++r) {
            auto& fields = shape.resources[r].fields;
            const auto& before = old.resources[r].fields;
            if (((fields[0] ^ before[0]) & 3u) != 0 || ((fields[1] ^ before[1]) & 0xffff0000u) != 0) return refuse(RebaseVertexBase);
            if (fields[3] != before[3]) return refuse(RebaseVertexFormat);
            if ((fields[2] == 0) != (before[2] == 0)) return refuse(RebaseVertexInfo);
            fields[0] = before[0];
            fields[1] = before[1];
            fields[2] = before[2];
        }
        if (!sameVertexInfo(shape, old)) return refuse(RebaseVertexInfo);
        for (const auto& attribute : compiled.vertexAttributes) {
            if (attribute.location >= info.resourcesNum || attribute.location >= info.resources.size() || attribute.resource.fields != old.resources[attribute.location].fields) return refuse(RebaseVertexInfo);
        }
        auto dropped = variant.decodeRanges;
        std::vector<std::pair<std::size_t, std::size_t>> kept;
        std::size_t offset = 0;
        for (std::size_t run = 0; run < variant.runs.size(); ++run) {
            const auto [begin, end] = variant.runs[run];
            const auto count = static_cast<std::size_t>((end - begin) / sizeof(std::uint32_t));
            const auto decodedRun = std::find_if(dropped.begin(), dropped.end(), [&](const auto& range) { return range.first == begin && range.first + range.second == end; });
            if (decodedRun != dropped.end()) {
                dropped.erase(decodedRun);
            } else {
                if (offset + count > variant.words.size()) return refuse(RebaseVertexLayout);
                kept.emplace_back(run, offset);
            }
            offset += count;
        }
        if (!dropped.empty()) return refuse(RebaseVertexLayout);
        struct Source {
            std::uint64_t begin, end;
            const std::uint32_t* words;
            std::size_t count;
        };
        std::vector<Source> sources;
        sources.reserve(kept.size() + decoded.reads.size());
        for (const auto& [run, at] : kept) {
            const auto [begin, end] = variant.runs[run];
            sources.push_back({begin, end, variant.words.data() + at, static_cast<std::size_t>((end - begin) / sizeof(std::uint32_t))});
        }
        std::vector<std::uint32_t> readWords;
        for (const auto& read : decoded.reads) readWords.resize(readWords.size() + read.size / sizeof(std::uint32_t));
        std::size_t readAt = 0;
        for (const auto& read : decoded.reads) {
            const auto count = static_cast<std::size_t>(read.size / sizeof(std::uint32_t));
            std::memcpy(readWords.data() + readAt, read.data.data(), count * sizeof(std::uint32_t));
            sources.push_back({read.address, read.address + read.size, readWords.data() + readAt, count});
            copy->decodeRanges.emplace_back(read.address, read.size);
            readAt += count;
        }
        std::stable_sort(sources.begin(), sources.end(), [](const Source& a, const Source& b) { return a.begin < b.begin; });
        copy->runs.reserve(sources.size());
        copy->words.reserve(offset);
        for (const auto& source : sources) {
            copy->runs.emplace_back(source.begin, source.end);
            copy->words.insert(copy->words.end(), source.words, source.words + source.count);
        }
        copy->vertexInfo = std::make_shared<const ShaderRecompiler::ShaderVertexStageInfo>(info);
    } else {
        copy->runBases = variant.runBases;
        copy->runs = variant.runs;
        copy->words = variant.words;
        copy->decodeRanges = variant.decodeRanges;
        for (auto left = relocated; left != 0; left &= left - 1) {
            const auto w = std::countr_zero(left);
            const auto delta = AlignedWord(now[static_cast<std::size_t>(w)]) - AlignedWord(was[static_cast<std::size_t>(w)]);
            for (std::size_t run = 0; run < copy->runs.size() && run < copy->runBases.size(); ++run) {
                if (copy->runBases[run] != static_cast<std::int8_t>(w)) continue;
                copy->runs[run].first += delta;
                copy->runs[run].second += delta;
            }
        }
    }
    auto result = std::make_shared<ShaderRecompiler::RecompileResult>(compiled);
    for (std::size_t slot = 0; slot < forwarded.size(); ++slot) {
        if (!forwardedMoved(slot)) continue;
        const auto value = now[forwarded[slot]];
        const auto at = slot * sizeof(std::uint32_t);
        if (at + sizeof(std::uint32_t) <= result->pushConstants.size()) std::memcpy(result->pushConstants.data() + at, &value, sizeof(value));
        for (auto& binding : result->bindings) {
            if (binding.role == ShaderRecompiler::DescriptorRole::ShaderData && slot < binding.guestDescriptor.size()) binding.guestDescriptor[slot] = value;
        }
    }
    for (const auto& patch : patches) result->bindings[patch.binding].guestDescriptor[patch.word] = patch.value;
    if (copy->vertexInfo != nullptr) {
        for (auto& attribute : result->vertexAttributes) attribute.resource = copy->vertexInfo->resources[attribute.location];
    }
    copy->shared = variant.shared;
    copy->relocatableWords = variant.relocatableWords;
    copy->valueWords = variant.valueWords;
    copy->baseWords = variant.baseWords;
    copy->forgetSerial = variant.forgetSerial;
    copy->compiled = std::move(result);
    copy->meshIndexDescriptor = variant.meshIndexDescriptor;
    copy->shader = variant.shader;
    copy->dataPositions = variant.dataPositions;
    copy->dataSlots = variant.dataSlots;
    copy->flatBinding = variant.flatBinding;
    copy->pushOffset = variant.pushOffset;
    copy->userDataMask = variant.userDataMask;
    copy->userData.assign(now.begin(), now.end());
    counters.built.fetch_add(1, std::memory_order_relaxed);
    return copy;
}

void Driver::applyShaderDataPatches(DispatchVariant& variant) {
    if (variant.shaderDataPatches.empty()) return;
    auto rebased = std::make_shared<ShaderRecompiler::RecompileResult>(*variant.compiled);
    for (const auto& [slot, value] : variant.shaderDataPatches) {
        const auto at = static_cast<std::size_t>(slot) * sizeof(std::uint32_t);
        if (at + sizeof(std::uint32_t) <= rebased->pushConstants.size()) std::memcpy(rebased->pushConstants.data() + at, &value, sizeof(value));
        for (auto& binding : rebased->bindings) {
            if (binding.role == ShaderRecompiler::DescriptorRole::ShaderData && slot < binding.guestDescriptor.size()) binding.guestDescriptor[slot] = value;
        }
    }
    variant.compiled = std::move(rebased);
    variant.shaderDataPatches.clear();
}

void Driver::reportDrawRelocation() {
    auto& counters = drawRelocationCounters;
    AgcDriver::ProfilePrint_nid_no_patch("[draw-relocate] %llu alias lookups: %llu donors with moved user words, %llu refusals, %llu hits through a relocated entry; %zu shader sets learned, %zu aliases held (%llu dropped at the limit)\n", static_cast<unsigned long long>(counters.lookups), static_cast<unsigned long long>(counters.donors), static_cast<unsigned long long>(counters.refused), static_cast<unsigned long long>(counters.hits), drawRelocations.size(), drawAliases.size(), static_cast<unsigned long long>(counters.evicted));
    counters = {};
}

}
