#include "Optimization/VectorWindowForwarder.hpp"
#include <cstdint>
#include <unordered_map>

namespace ShaderRecompiler {

VectorWindowForwardingStats VectorWindowForwarder::Forward(IrProgram& program) const {
    VectorWindowForwardingStats stats;
    if (program.Metadata().vectorWindowSize == 0u) return stats;
    std::unordered_map<std::uint32_t, IrValue*> known;
    for (auto* block : program.BlockOrder()) {
        known.clear();
        for (auto* inst : block->Instructions()) {
            if (inst->HasImmediate()) continue;
            const bool load = inst->Opcode() == IrOpcode::LoadVectorWindow;
            if (!load && inst->Opcode() != IrOpcode::StoreVectorWindow) continue;
            const IrValue* index = inst->Argument(0)->Resolve();
            if (!index->HasImmediate()) {
                if (!load) known.clear();
                continue;
            }
            const auto element = index->ImmediateU32();
            if (!load) {
                known[element] = inst->Argument(1)->Resolve();
                continue;
            }
            const auto [found, inserted] = known.try_emplace(element, inst);
            if (inserted) continue;
            inst->ReplaceAllUsesWith(found->second);
            ++stats.forwardedLoads;
        }
    }
    return stats;
}

}
