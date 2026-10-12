#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_VECTORWINDOWFORWARDER_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_VECTORWINDOWFORWARDER_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include <cstdint>

namespace ShaderRecompiler {

struct VectorWindowForwardingStats {
    std::uint32_t forwardedLoads = 0;
};

class VectorWindowForwarder {
public:
    [[nodiscard]] VectorWindowForwardingStats Forward(IrProgram& program) const;
};

}

#endif
