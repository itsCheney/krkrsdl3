#pragma once
#include "LayerRenderOperation.h"
#include <string>
namespace krkrsdl3 { class iTVPRenderBackend; }
// The facade and software methods have process lifetime; only the binding is
// session scoped. Unbind after clearing layers and recycling deleted textures.
bool TVPBindMetalLayerRenderManager(krkrsdl3::iTVPRenderBackend* backend);
void TVPUnbindMetalLayerRenderManager();
const char* TVPMetalLayerFallbackReason();
bool TVPMetalLayerCompositionActive();
TVPLayerRenderStats TVPGetMetalLayerRenderStats();
std::string TVPGetMetalLayerMultipleInputMethodSummary();
std::string TVPGetMetalLayerUnsupportedMethodSummary();
