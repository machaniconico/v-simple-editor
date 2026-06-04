#pragma once

#include <cstdlib>
#include <cstring>

// VEDITOR_HDR_IDT_GPU — preview の per-clip 入力色空間変換 (IDT) を CPU
// (clipcolor::toUnifiedSpace) でなく GPU フラグメントシェーダ
// (GpuLayerCompositor::composite16Idt) で行うかどうかのマスターフラグ。
// 既定 OFF。ON でも clipidt (VEDITOR_HDR_IDT) が OFF なら何も起きない
// (IDT 自体が無効)。ODT (VEDITOR_HDR_ODT) ON 時は CPU toLinearWorking +
// applyOdt16 経路が優先され GPU IDT 経路は使われない (Story1 スコープ=
// matte-free ∧ IDT-only)。

namespace idtgpu {

inline bool enabledFromEnv()
{
    const char* v = std::getenv("VEDITOR_HDR_IDT_GPU");
    return v && v[0] && std::strcmp(v, "0") != 0;
}

} // namespace idtgpu
