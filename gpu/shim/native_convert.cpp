// SPDX-License-Identifier: GPL-2.0-or-later
// vwgpu_native_convert: every vertex and pixel shader in the game's executable through the
// recompiler, ahead of time. The native renderer (src/native_vk.c) runs this on the player's
// machine from their own dump and loads OUT_DIR/<hash>.spv; no converted shader ships.
//
// Each Gnm shader sits in the executable as a container: a "Shdr" header with the stage's
// registers, input-usage slots (which game slot sits in which user register or table) and
// semantics, then the GCN code ending in an "OrbShdr" footer. The recompiler normally reads the
// game's live registers and resource tables; here they are made up from the header: every
// constant buffer, texture and vertex buffer descriptor gets a marker address that names its
// game slot, so the slot of each resource in the output is known by construction.
//
// Also writes one JSON line per shader to OUT_DIR/index.jsonl:
//   {"hash", "stage" (vs/ps), "buffers": [game slot or -1 special], "images": [slot],
//    "samplers": [slot], "inputs": n, "exports": [semantic per VS output],
//    "ps_inputs": [semantic per PS input]}
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/regs_depth.h"
#include "video_core/amdgpu/regs_shader.h"
#include "video_core/amdgpu/resource.h"

#include "../vwgpu.h"

namespace {

using namespace Shader;

// Marker addresses: kind << 32 | slot << 20 (buffers), images by their 38-bit field.
constexpr u64 MarkConst = 1, MarkVertex = 2, MarkRw = 3;
u64 BufferMark(u64 kind, u32 slot) {
    return kind << 32 | u64(slot) << 20;
}
int SlotOfBuffer(u64 address) {
    if ((address >> 32) != MarkConst) {
        return (address >> 32) == MarkVertex ? 100 + int((address >> 20) & 0xfff) : -1;
    }
    return int((address >> 20) & 0xfff);
}
constexpr u64 ImageMark = 0x30000000;

AmdGpu::Buffer MakeBuffer(u64 kind, u32 slot, u32 stride, u32 elements) {
    AmdGpu::Buffer b{};
    b.base_address = BufferMark(kind, slot);
    b.stride = stride;
    b.num_records = 1u << 20;
    // Channels past the format's read as (0, 0, 0, 1), like the game's own V#s.
    b.dst_sel_x = 4;
    b.dst_sel_y = elements > 1 ? 5 : 0;
    b.dst_sel_z = elements > 2 ? 6 : 0;
    b.dst_sel_w = elements > 3 ? 7 : 1;
    b.num_format = u32(AmdGpu::NumberFormat::Float);
    b.data_format = elements == 1   ? u32(AmdGpu::DataFormat::Format32)
                    : elements == 2 ? u32(AmdGpu::DataFormat::Format32_32)
                    : elements == 3 ? u32(AmdGpu::DataFormat::Format32_32_32)
                                    : u32(AmdGpu::DataFormat::Format32_32_32_32);
    return b;
}
AmdGpu::Image MakeImage(u32 slot) {
    AmdGpu::Image image = AmdGpu::Image::Null(false);
    image.base_address = ImageMark + slot;
    image.width = 255;
    image.height = 255;
    image.pitch = 255;
    image.type = u64(AmdGpu::ImageType::Color2D);
    image.tiling_index = 13;
    return image;
}

struct Slot {
    u8 type, api, reg, flags;
};
enum : u8 {
    ImmResource = 0, ImmSampler = 1, ImmConstBuffer = 2, ImmVertexBuffer = 3, ImmRwResource = 4,
    SubPtrFetchShader = 0x12, PtrResourceTable = 0x13, PtrSamplerTable = 0x15,
    PtrConstBufferTable = 0x16, PtrVertexBufferTable = 0x17, PtrRwResourceTable = 0x19,
    PtrExtendedUserData = 0x1b,
};

struct Parsed {
    u64 hash;
    bool vs;
    std::span<const u32> code;
    u32 regs[12];
    std::vector<Slot> slots;
    std::vector<u32> input_semantics; // VS: {semantic, vgpr, size}; PS: 16-bit entries
    std::vector<u16> export_semantics;
};

// Fake guest memory: the register file (16 user registers, extended user data after them),
// resource tables and a dummy fetch shader, all host memory the recompiler reads directly.
struct Guest {
    std::vector<u32> file = std::vector<u32>(256);
    std::vector<std::vector<u32>> tables;
    u32 fetch_code[4] = {0xbe802000, 0, 0, 0}; // s_setpc_b64 s[0:1]
    u32* Table(u32 dwords) {
        return tables.emplace_back(dwords).data();
    }
    void Pointer(u32 reg, const void* p) {
        const u64 v = reinterpret_cast<u64>(p);
        file[reg] = u32(v);
        file[reg + 1] = u32(v >> 32);
    }
    template <typename T>
    void Put(u32* at, const T& value) {
        std::memcpy(at, &value, sizeof(T));
    }
};

std::string Json(const std::vector<int>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        s += (i ? "," : "") + std::to_string(v[i]);
    }
    return s + "]";
}

bool native = false; // SPIR-V for the native renderer (Profile::native_renderer)

bool Compile(const Parsed& p, const std::string& out_dir, std::FILE* index) {
    Guest g;
    // Tables are indexed by game slot: T# 8 dwords, S# 4, V# 4.
    for (const Slot& s : p.slots) {
        u32* at = &g.file[s.reg];
        switch (s.type) {
        case ImmResource:
        case ImmRwResource:
            g.Put(at, MakeImage(s.api));
            break;
        case ImmSampler:
            break;
        case ImmConstBuffer:
            g.Put(at, MakeBuffer(MarkConst, s.api, 0, 4));
            break;
        case ImmVertexBuffer:
            g.Put(at, MakeBuffer(MarkVertex, s.api, 16, 4));
            break;
        case SubPtrFetchShader:
            g.Pointer(s.reg, g.fetch_code);
            break;
        case PtrResourceTable:
        case PtrRwResourceTable: {
            u32* t = g.Table(8 * 128);
            for (u32 k = 0; k < 128; ++k) {
                g.Put(t + 8 * k, MakeImage(k));
            }
            g.Pointer(s.reg, t);
            break;
        }
        case PtrSamplerTable:
            g.Pointer(s.reg, g.Table(4 * 128));
            break;
        case PtrConstBufferTable: {
            u32* t = g.Table(4 * 128);
            for (u32 k = 0; k < 128; ++k) {
                g.Put(t + 4 * k, MakeBuffer(MarkConst, k, 0, 4));
            }
            g.Pointer(s.reg, t);
            break;
        }
        case PtrVertexBufferTable: {
            u32* t = g.Table(4 * 128);
            for (u32 k = 0; k < 128; ++k) {
                g.Put(t + 4 * k, MakeBuffer(MarkVertex, k, 16, 4));
            }
            g.Pointer(s.reg, t);
            break;
        }
        case PtrExtendedUserData:
            g.Pointer(s.reg, &g.file[16]);
            break;
        default:
            std::fprintf(stderr, "%08x: usage type %#x not handled\n", u32(p.hash), s.type);
            return false;
        }
    }

    const HwStage hw = p.vs ? HwStage::Vertex : HwStage::Fragment;
    const SwStage sw = p.vs ? SwStage::Vertex : SwStage::Fragment;
    ShaderParams params{.user_data = std::span<const u32, 16>{g.file.data(), 16},
                        .code = p.code,
                        .hash = p.hash};
    Info info{hw, sw, params};
    RuntimeInfo runtime{};
    runtime.Initialize(hw, sw);
    AmdGpu::ShaderProgram program{};
    const u64 settings = u64(p.regs[3]) << 32 | p.regs[2];
    std::memcpy(&program.settings, &settings, sizeof(settings));
    runtime.props.num_user_data = program.settings.num_user_regs;
    runtime.props.num_input_vgprs = program.settings.vgpr_comp_cnt;
    runtime.props.num_allocated_vgprs = program.NumVgprs();
    runtime.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
    runtime.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
    runtime.props.fp_round_mode32 = program.settings.fp_round_mode32;
    runtime.props.fp_round_mode16_64 = program.settings.fp_round_mode64;

    // VS: the vertex inputs as the game's fetch shader would load them: attribute i from vertex
    // buffer slot i into the semantic's VGPRs.
    Gcn::FetchShaderSnapshot snapshot;
    std::vector<int> ps_inputs, exports;
    if (p.vs) {
        if (p.regs[6] != 0) {
            std::fprintf(stderr, "%08x: PA_CL_VS_OUT_CNTL %#x not handled\n", u32(p.hash), p.regs[6]);
            return false;
        }
        u32 table_reg = 0;
        for (const Slot& s : p.slots) {
            if (s.type == PtrVertexBufferTable) {
                table_reg = s.reg;
            }
        }
        Gcn::FetchShaderData fetch{};
        for (u32 i = 0; i < p.input_semantics.size(); ++i) {
            const u32 sem = p.input_semantics[i];
            auto& a = fetch.attributes.emplace_back();
            a.semantic = u8(i);
            a.dest_vgpr = u8(sem >> 8);
            a.num_elements = u8(sem >> 16);
            a.sgpr_base = u8(table_reg);
            a.dword_offset = u8(4 * i);
            snapshot.buffers.push_back(MakeBuffer(MarkVertex, i, 4 * a.num_elements, a.num_elements));
        }
        snapshot.info = &info;
        snapshot.fetch = fetch;
        for (u16 e : p.export_semantics) {
            exports.push_back(e & 0xff);
        }
    } else {
        auto& fs = runtime.hw.fs;
        std::memcpy(&fs.en_flags, &p.regs[6], sizeof(u32));
        std::memcpy(&fs.addr_flags, &p.regs[7], sizeof(u32));
        fs.num_inputs = p.regs[8] & 0x3f;
        if (fs.num_inputs != p.input_semantics.size()) {
            std::fprintf(stderr, "%08x: %u interpolants, %zu semantics\n", u32(p.hash),
                         fs.num_inputs, p.input_semantics.size());
            return false;
        }
        for (u32 i = 0; i < fs.num_inputs; ++i) {
            const u32 sem = p.input_semantics[i];
            // The input's location is its semantic; aot.py compacts them.
            fs.inputs[i] = {.param_index = u8((sem & 0xff) - (native ? 15 : 0)), .is_default = false,
                            .is_flat = bool((sem >> 10) & 1), .default_value = 0};
            ps_inputs.push_back(int(sem & 0xff));
        }
        fs.z_export_format = AmdGpu::ShaderExportFormat(p.regs[4]);
        AmdGpu::DepthShaderControl db{};
        std::memcpy(&db, &p.regs[10], sizeof(u32));
        fs.mrtz_mask = db.z_export_enable | (db.stencil_op_val_export_enable | db.stencil_test_val_export_enable) << 1 |
                       db.mask_export_enable << 2 | db.coverage_to_mask_enable << 3;
        const AmdGpu::ColorExportFormat col{p.regs[5]};
        for (u32 i = 0; i < MaxColorBuffers; ++i) {
            if (col.GetFormat(i) == AmdGpu::ShaderExportFormat::Zero) {
                continue;
            }
            auto& cb = fs.color_buffers[i];
            cb.data_format = AmdGpu::DataFormat::Format8_8_8_8;
            cb.num_format = AmdGpu::NumberFormat::Unorm;
            cb.num_conversion = AmdGpu::NumberConversion::None;
            cb.export_format = col.GetFormat(i);
            cb.swizzle = AmdGpu::IdentityMapping;
        }
        fs.num_samples = 1;
    }

    Profile profile{};
    profile.max_viewport_width = 16384;
    profile.max_viewport_height = 16384;
    profile.max_shared_memory_size = 32768;
    profile.supported_spirv = 0x00010600;
    profile.subgroup_size = 64;
    profile.support_int8 = profile.support_int16 = profile.support_int64 = true;
    profile.support_float16 = true;
    profile.native_renderer = native;

    try {
        Gcn::ScopedFetchShaderSnapshot scoped{snapshot};
        Pools pools;
        auto ir = TranslateProgram(p.code, pools, info, runtime, profile);
        if (info.translation_failed) {
            std::fprintf(stderr, "%08x: translation failed\n", u32(p.hash));
            return false;
        }
        if (native) {
            // Constant blocks by game slot (pixel shader slots after the vertex shader's),
            // textures by game slot, outputs at semantic - 15 (the pixel side matches).
            auto& layout = info.native;
            for (size_t i = 0; i < info.buffers.size(); ++i) {
                const auto& b = info.buffers[i];
                const u64 address = b.IsSpecial() ? 0 : b.GetSharp(info).base_address;
                layout.buffer_block[i] =
                    (address >> 32) == MarkConst
                        ? s8(SlotOfBuffer(address) + (p.vs ? 0 : Info::NativeLayout::BlocksPerStage))
                        : -1;
            }
            for (size_t i = 0; i < info.images.size(); ++i) {
                layout.image_slot[i] = s8(info.images[i].GetSharp(info).base_address - ImageMark);
            }
            for (size_t i = 0; i < p.export_semantics.size(); ++i) {
                layout.output_location[i] = u8((p.export_semantics[i] & 0xff) - 15);
            }
        }
        Backend::Bindings binding{};
        const auto spv = Backend::SPIRV::EmitSPIRV(profile, runtime, ir, binding);
        char name[32];
        std::snprintf(name, sizeof(name), "%08x", u32(p.hash));
        std::ofstream(out_dir + "/" + name + ".spv", std::ios::binary)
            .write(reinterpret_cast<const char*>(spv.data()), spv.size() * 4);
        std::vector<int> buffers, images, samplers;
        for (const auto& b : info.buffers) {
            buffers.push_back(b.IsSpecial() ? -1 : SlotOfBuffer(b.GetSharp(info).base_address));
        }
        for (const auto& im : info.images) {
            const u64 address = im.GetSharp(info).base_address;
            images.push_back(address >= ImageMark ? int(address - ImageMark) : -1);
        }
        // Samplers have no address to mark; the native renderer pairs each texture with the
        // sampler of its slot.
        samplers.assign(info.samplers.size(), -1);
        std::fprintf(index,
                     "{\"hash\":\"%s\",\"stage\":\"%s\",\"buffers\":%s,\"images\":%s,"
                     "\"samplers\":%s,\"inputs\":%zu,\"exports\":%s,\"ps_inputs\":%s}\n",
                     name, p.vs ? "vs" : "ps", Json(buffers).c_str(), Json(images).c_str(),
                     Json(samplers).c_str(), p.vs ? p.input_semantics.size() : size_t(0),
                     Json(exports).c_str(), Json(ps_inputs).c_str());
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%08x: %s\n", u32(p.hash), e.what());
        return false;
    }
}

int ConvertAll(const char* eboot, const std::string& out_dir) {
    std::ifstream file(eboot, std::ios::binary);
    if (!file) {
        return -1;
    }
    std::vector<char> raw((std::istreambuf_iterator<char>(file)), {});
    // Keep the code 4-byte aligned: the recompiler reads it as u32 words.
    std::vector<u32> words((raw.size() + 3) / 4);
    std::memcpy(words.data(), raw.data(), raw.size());
    const u8* d = reinterpret_cast<const u8*>(words.data());
    const size_t size = raw.size();
    std::error_code error;
    std::filesystem::create_directories(out_dir, error);
    std::FILE* index = std::fopen((out_dir + "/index.jsonl").c_str(), "w");
    if (!index) {
        return -1;
    }
    std::set<u64> done;
    int ok = 0, failed = 0, skipped = 0;
    for (size_t o = 0; o + 28 <= size; o += 4) {
        if (std::memcmp(d + o, "OrbShdr", 7) != 0) {
            continue;
        }
        const AmdGpu::BinaryInfo& bin = *reinterpret_cast<const AmdGpu::BinaryInfo*>(d + o);
        if (bin.type != 0 && bin.type != 1) {
            ++skipped;
            continue;
        }
        if (!done.insert(bin.shader_hash).second) {
            continue;
        }
        // The code starts with s_mov_b32 vcc_hi, n where (n + 1) * 8 bytes reach the footer.
        size_t start = 0;
        for (size_t c = o - 8; c + 0x10000 > o && c > 0; c -= 4) {
            const u32* w = reinterpret_cast<const u32*>(d + c);
            if (w[0] == 0xbeeb03ff && c + (size_t(w[1]) + 1) * 8 == o) {
                start = c;
                break;
            }
        }
        // The container header: the closest "Shdr" before the code.
        size_t hdr = 0;
        for (size_t c = start; start && c + 0x400 > start && c >= 4; c -= 4) {
            if (std::memcmp(d + c, "Shdr", 4) == 0 && std::memcmp(d + c - 3, "Orb", 3) != 0) {
                hdr = c;
                break;
            }
        }
        if (!start || !hdr) {
            std::fprintf(stderr, "%08x: code or header not found\n", u32(bin.shader_hash));
            ++failed;
            continue;
        }
        Parsed p{};
        p.hash = bin.shader_hash;
        p.vs = bin.type == 1;
        p.code = std::span<const u32>(reinterpret_cast<const u32*>(d + start), bin.length / 4);
        const u32 common = *reinterpret_cast<const u32*>(d + hdr + 0x10);
        const u32 num_slots = common >> 24;
        const u32 num_regs = p.vs ? 7 : 12;
        std::memcpy(p.regs, d + hdr + 0x18, num_regs * 4);
        const u8* counts = d + hdr + 0x18 + num_regs * 4;
        const u8* at = counts + 4;
        for (u32 i = 0; i < num_slots; ++i, at += 4) {
            p.slots.push_back({at[0], at[1], at[2], at[3]});
        }
        if (p.vs) {
            for (u32 i = 0; i < counts[0]; ++i, at += 4) {
                p.input_semantics.push_back(at[0] | at[1] << 8 | at[2] << 16);
            }
            for (u32 i = 0; i < counts[1]; ++i, at += 2) {
                p.export_semantics.push_back(u16(at[0] | at[1] << 8));
            }
        } else {
            for (u32 i = 0; i < counts[0]; ++i, at += 2) {
                p.input_semantics.push_back(u32(at[0] | at[1] << 8));
            }
        }
        if (at > d + start) {
            std::fprintf(stderr, "%08x: header runs into the code\n", u32(bin.shader_hash));
            ++failed;
            continue;
        }
        (Compile(p, out_dir, index) ? ok : failed)++;
    }
    std::fclose(index);
    std::printf("Native: %d of the game's shaders converted, %d failed, %d not vertex or pixel shaders\n",
                ok, failed, skipped);
    return ok;
}

} // namespace

extern "C" int vwgpu_native_convert(const char* eboot, const char* out_dir, int native_renderer) {
    native = native_renderer != 0;
    return ConvertAll(eboot, out_dir);
}
