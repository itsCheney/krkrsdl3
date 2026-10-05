#pragma once

// Row expansions are reusable by the extension fixture. No build-generated
// definition is needed: C++, runtime MSL and diagnostics consume the same .def.
#define TVP_LAYER_MSL_CONSTANT(name, id, ...) \
    "constant int TVP_LAYER_KIND_" #name " = " #id ";\n"
#define TVP_LAYER_MSL_NEEDS_SOURCE(name, id, mask, inputs, ...) \
    "case " #id ": return " #inputs " != 0;\n"
#define TVP_LAYER_MSL_READS_TARGET(name, id, mask, inputs, f0, f1, f2, reference, reads, ...) \
    "case " #id ": return " #reads ";\n"

inline constexpr char TVP_LAYER_OPERATION_MSL_DEFINITIONS[] =
#define TVP_LAYER_OPERATION TVP_LAYER_MSL_CONSTANT
#define TVP_LAYER_OPERATION_COUNT(count)
#include "LayerOperationDefinitions.def"
#undef TVP_LAYER_OPERATION
    "bool layerNeedsSource(int kind) { switch(kind) {\n"
#define TVP_LAYER_OPERATION TVP_LAYER_MSL_NEEDS_SOURCE
#include "LayerOperationDefinitions.def"
#undef TVP_LAYER_OPERATION
    "default: return false; } }\n"
    "bool layerReadsTarget(int kind) { switch(kind) {\n"
#define TVP_LAYER_OPERATION TVP_LAYER_MSL_READS_TARGET
#include "LayerOperationDefinitions.def"
#undef TVP_LAYER_OPERATION
#undef TVP_LAYER_OPERATION_COUNT
    "default: return false; } }\n";
