#include <Geode/Geode.hpp>

#ifdef GEODE_IS_WINDOWS

#include <Windows.h>
#include <Geode/cocos/kazmath/include/kazmath/GL/matrix.h>
#include <Geode/cocos/shaders/CCGLProgram.h>
#include <Geode/cocos/shaders/ccGLStateCache.h>
#include <Geode/cocos/textures/CCTextureAtlas.h>
#include <Geode/cocos/textures/CCTexture2D.h>
#include <Geode/cocos/include/ccTypes.h>
#include <Geode/cocos/platform/win32/CCGL.h>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include "config.hpp"
#include "gpu_cull.hpp"
#include "rendering.hpp"

// GPU per-quad batch culling. See gpu_cull.hpp for the design. This file contains the
// GL 4.3 pieces only: entry points are resolved at runtime (cocos ships a GLEW limited
// to GL 4.2 and has no GL 4.3 symbols), every failure latches to the CPU path, and the
// draw goes through glDrawElementsIndirect so no GPU result is ever read back by the CPU.
//
// The compute shader and the index pipeline are written from scratch for this mod;
// Bismuth (GPL) was used as a design reference only and no code or shaders are shared.

namespace reborn {

    namespace {

        // --- GL constants missing from cocos's GLEW (they are all GL 4.3) -------------
        unsigned int const kGlComputeShader = 0x91B9;
        unsigned int const kGlShaderStorageBuffer = 0x90D2;
        unsigned int const kGlShaderStorageBarrierBit = 0x00002000;
        unsigned int const kGlCommandBarrierBit = 0x00000040;
        unsigned int const kGlElementArrayBarrierBit = 0x00000010;
        // GL_VERSION is in glew.h; the rest of the 4.3 names get resolved as pointers.
        unsigned int const kGlVersion = 0x1F02;

        using GlGetString = GLubyte const* (APIENTRY*)(unsigned int name);
        using GlDispatchCompute = void (APIENTRY*)(unsigned int groupsX, unsigned int groupsY, unsigned int groupsZ);
        using GlGenBuffers = void (APIENTRY*)(int n, unsigned int* buffers);
        using GlBindBuffer = void (APIENTRY*)(unsigned int target, unsigned int buffer);
        using GlBufferData = void (APIENTRY*)(unsigned int target, ptrdiff_t size, void const* data, unsigned int usage);
        using GlBufferSubData = void (APIENTRY*)(unsigned int target, ptrdiff_t offset, ptrdiff_t size, void const* data);
        using GlBindBufferBase = void (APIENTRY*)(unsigned int target, unsigned int index, unsigned int buffer);
        using GlDrawElementsIndirect = void (APIENTRY*)(unsigned int mode, unsigned int type, void const* indirect);
        using GlMemoryBarrier = void (APIENTRY*)(unsigned int barriers);
        using GlCreateShader = unsigned int (APIENTRY*)(unsigned int type);
        using GlShaderSource = void (APIENTRY*)(unsigned int shader, int count, char const* const* string, int const* length);
        using GlCompileShader = void (APIENTRY*)(unsigned int shader);
        using GlGetShaderiv = void (APIENTRY*)(unsigned int shader, unsigned int pname, int* params);
        using GlDeleteShader = void (APIENTRY*)(unsigned int shader);
        using GlCreateProgram = unsigned int (APIENTRY*)(void);
        using GlAttachShader = void (APIENTRY*)(unsigned int program, unsigned int shader);
        using GlLinkProgram = void (APIENTRY*)(unsigned int program);
        using GlGetProgramiv = void (APIENTRY*)(unsigned int program, unsigned int pname, int* params);
        using GlDeleteProgram = void (APIENTRY*)(unsigned int program);
        using GlUseProgram = void (APIENTRY*)(unsigned int program);
        using GlGetUniformLocation = int (APIENTRY*)(unsigned int program, char const* name);
        using GlUniformMatrix4fv = void (APIENTRY*)(int location, int count, unsigned char transpose, float const* value);
        using GlUniform1ui = void (APIENTRY*)(int location, unsigned int v0);

        GlGetString pGetString = nullptr;
        GlDispatchCompute pDispatchCompute = nullptr;
        GlGenBuffers pGenBuffers = nullptr;
        GlBindBuffer pBindBuffer = nullptr;
        GlBufferData pBufferData = nullptr;
        GlBufferSubData pBufferSubData = nullptr;
        GlBindBufferBase pBindBufferBase = nullptr;
        GlDrawElementsIndirect pDrawElementsIndirect = nullptr;
        GlMemoryBarrier pMemoryBarrier = nullptr;
        GlCreateShader pCreateShader = nullptr;
        GlShaderSource pShaderSource = nullptr;
        GlCompileShader pCompileShader = nullptr;
        GlGetShaderiv pGetShaderiv = nullptr;
        GlDeleteShader pDeleteShader = nullptr;
        GlCreateProgram pCreateProgram = nullptr;
        GlAttachShader pAttachShader = nullptr;
        GlLinkProgram pLinkProgram = nullptr;
        GlGetProgramiv pGetProgramiv = nullptr;
        GlDeleteProgram pDeleteProgram = nullptr;
        GlUseProgram pUseProgram = nullptr;
        GlGetUniformLocation pGetUniformLocation = nullptr;
        GlUniformMatrix4fv pUniformMatrix4fv = nullptr;
        GlUniform1ui pUniform1ui = nullptr;

        bool glewReady() {
            static bool done = false;
            static bool ok = false;
            if (done) return ok;
            done = true;

            HMODULE glew = GetModuleHandleA("glew32.dll");
            if (!glew) glew = LoadLibraryA("glew32.dll");
            if (!glew) return false;

            auto const proc = [glew](char const* name) -> FARPROC {
                return GetProcAddress(glew, name);
            };
            // cocos ships GLEW 2.3.4 with GL <= 4.2 symbols; every entry needed here exists
            // there except glDispatchCompute, which comes from wglGetProcAddress.
            auto const load = [&proc](char const* suffix) -> FARPROC {
                std::string name = "__glew";
                name += suffix;
                return proc(name.c_str());
            };

            HMODULE opengl = GetModuleHandleA("opengl32.dll");
            pGetString = reinterpret_cast<GlGetString>(
                opengl ? GetProcAddress(opengl, "glGetString") : nullptr
            );
            PROC const dispatchProc = wglGetProcAddress("glDispatchCompute");
            pDispatchCompute = reinterpret_cast<GlDispatchCompute>(dispatchProc);
            pGenBuffers = reinterpret_cast<GlGenBuffers>(load("GenBuffers"));
            pBindBuffer = reinterpret_cast<GlBindBuffer>(load("BindBuffer"));
            pBufferData = reinterpret_cast<GlBufferData>(load("BufferData"));
            pBufferSubData = reinterpret_cast<GlBufferSubData>(load("BufferSubData"));
            pBindBufferBase = reinterpret_cast<GlBindBufferBase>(load("BindBufferBase"));
            pDrawElementsIndirect = reinterpret_cast<GlDrawElementsIndirect>(load("DrawElementsIndirect"));
            pMemoryBarrier = reinterpret_cast<GlMemoryBarrier>(load("MemoryBarrier"));
            pCreateShader = reinterpret_cast<GlCreateShader>(load("CreateShader"));
            pShaderSource = reinterpret_cast<GlShaderSource>(load("ShaderSource"));
            pCompileShader = reinterpret_cast<GlCompileShader>(load("CompileShader"));
            pGetShaderiv = reinterpret_cast<GlGetShaderiv>(load("GetShaderiv"));
            pDeleteShader = reinterpret_cast<GlDeleteShader>(load("DeleteShader"));
            pCreateProgram = reinterpret_cast<GlCreateProgram>(load("CreateProgram"));
            pAttachShader = reinterpret_cast<GlAttachShader>(load("AttachShader"));
            pLinkProgram = reinterpret_cast<GlLinkProgram>(load("LinkProgram"));
            pGetProgramiv = reinterpret_cast<GlGetProgramiv>(load("GetProgramiv"));
            pDeleteProgram = reinterpret_cast<GlDeleteProgram>(load("DeleteProgram"));
            pUseProgram = reinterpret_cast<GlUseProgram>(load("UseProgram"));
            pGetUniformLocation = reinterpret_cast<GlGetUniformLocation>(load("GetUniformLocation"));
            pUniformMatrix4fv = reinterpret_cast<GlUniformMatrix4fv>(load("UniformMatrix4fv"));
            pUniform1ui = reinterpret_cast<GlUniform1ui>(load("Uniform1ui"));

            ok = pGetString && pDispatchCompute && pGenBuffers && pBindBuffer && pBufferData
                && pBufferSubData && pBindBufferBase && pDrawElementsIndirect && pMemoryBarrier
                && pCreateShader && pShaderSource && pCompileShader && pGetShaderiv && pDeleteShader
                && pCreateProgram && pAttachShader && pLinkProgram && pGetProgramiv && pDeleteProgram
                && pUseProgram && pGetUniformLocation && pUniformMatrix4fv && pUniform1ui;
            return ok;
        }

        // Written from scratch for this mod. One invocation per quad; identical tests to the
        // CPU filter (screen-space MVP, same clip margin, same area epsilon, same index
        // order tl, bl, tr, br, tr, bl). Any doubt fails open to keeping the quad.
        char const* kCullShaderSource = R"(#version 430

layout(local_size_x = 64) in;

layout(std430, binding = 0) buffer VertexWords {
    uint words[];
};

layout(std430, binding = 1) buffer IndirectCommand {
    uint count;
    uint instanceCount;
    uint firstIndex;
    uint baseVertex;
    uint baseInstance;
};

layout(std430, binding = 2) buffer IndexWords {
    uint indices[];
};

uniform mat4 uMvp;
uniform uint uTotalQuads;

const float kNdcMargin = 0.005;
const float kAreaEpsilon = 1e-12;

bool projectVertex(float x, float y, float z, out float ndcX, out float ndcY) {
    vec4 clip = uMvp * vec4(x, y, z, 1.0);
    // w <= 0 or NaN: fail open (keep the quad), matching the CPU filter's w <= 1e-6 rule.
    if (!(clip.w > 1e-6)) {
        return false;
    }
    float nx = clip.x / clip.w;
    float ny = clip.y / clip.w;
    if (nx != nx || ny != ny) {
        return false;
    }
    ndcX = nx;
    ndcY = ny;
    return true;
}

void keepQuad(uint quad) {
    uint slot = atomicAdd(count, 6u);
    uint base = quad * 4u;
    indices[slot + 0u] = base + 0u;  // tl
    indices[slot + 1u] = base + 1u;  // bl
    indices[slot + 2u] = base + 2u;  // tr
    indices[slot + 3u] = base + 3u;  // br
    indices[slot + 4u] = base + 2u;  // tr
    indices[slot + 5u] = base + 1u;  // bl
}

void main() {
    uint quad = gl_GlobalInvocationID.x;
    if (quad >= uTotalQuads) return;

    uint base = quad * 24u;  // one ccV3F_C4B_T2F is 6 words; one quad is 4 vertices

    float ndcX[4];
    float ndcY[4];
    bool usable = true;
    for (uint v = 0u; v < 4u; ++v) {
        uint word = base + v * 6u;
        float x = uintBitsToFloat(words[word + 0u]);
        float y = uintBitsToFloat(words[word + 1u]);
        float z = uintBitsToFloat(words[word + 2u]);
        if (!projectVertex(x, y, z, ndcX[v], ndcY[v])) {
            usable = false;
            break;
        }
    }

    if (usable) {
        float minX = min(min(ndcX[0], ndcX[1]), min(ndcX[2], ndcX[3]));
        float maxX = max(max(ndcX[0], ndcX[1]), max(ndcX[2], ndcX[3]));
        float minY = min(min(ndcY[0], ndcY[1]), min(ndcY[2], ndcY[3]));
        float maxY = max(max(ndcY[0], ndcY[1]), max(ndcY[2], ndcY[3]));

        bool outside =
            maxX < -1.0 - kNdcMargin || minX > 1.0 + kNdcMargin ||
            maxY < -1.0 - kNdcMargin || minY > 1.0 + kNdcMargin;

        float ax = ndcX[1] - ndcX[0];
        float ay = ndcY[1] - ndcY[0];
        float bx = ndcX[2] - ndcX[0];
        float by = ndcY[2] - ndcY[0];
        float area = abs(ax * by - ay * bx);

        if (outside || area < kAreaEpsilon) {
            return;  // culled: zero area or fully outside the clip volume
        }
    }

    keepQuad(quad);
}
)";

        enum class GlState { Untried, Ready, Failed };
        GlState s_state = GlState::Untried;  // main thread only

        unsigned int s_commandBuffer = 0;
        unsigned int s_indexBuffer = 0;
        unsigned int s_program = 0;
        int s_mvpLocation = -1;
        int s_totalLocation = -1;

        // 16383 kept quads * 6 indices * 4 bytes; mirrors the CPU filter's cap.
        constexpr std::size_t kMaxGpuQuads = 16383;
        constexpr unsigned int kLocalSizeX = 64;

        std::atomic<std::uint64_t> s_batchesSubmitted{0};
        std::atomic<std::uint64_t> s_quadsScanned{0};

        bool buildProgram() {
            unsigned int const shader = pCreateShader(kGlComputeShader);
            char const* source = kCullShaderSource;
            pShaderSource(shader, 1, &source, nullptr);
            pCompileShader(shader);
            int compiled = 0;
            pGetShaderiv(shader, 0x8B81 /* GL_COMPILE_STATUS */, &compiled);
            if (!compiled) {
                pDeleteShader(shader);
                log::warn("gpu batch cull: compute shader failed to compile; falling back to the CPU filter");
                return false;
            }

            unsigned int const program = pCreateProgram();
            pAttachShader(program, shader);
            pLinkProgram(program);
            pDeleteShader(shader);
            int linked = 0;
            pGetProgramiv(program, 0x8B82 /* GL_LINK_STATUS */, &linked);
            if (!linked) {
                pDeleteProgram(program);
                log::warn("gpu batch cull: compute program failed to link; falling back to the CPU filter");
                return false;
            }

            int const mvp = pGetUniformLocation(program, "uMvp");
            int const total = pGetUniformLocation(program, "uTotalQuads");
            if (mvp < 0 || total < 0) {
                pDeleteProgram(program);
                log::warn("gpu batch cull: compute uniforms missing; falling back to the CPU filter");
                return false;
            }

            s_program = program;
            s_mvpLocation = mvp;
            s_totalLocation = total;
            return true;
        }

        bool ensureReady() {
            if (s_state != GlState::Untried) return s_state == GlState::Ready;
            s_state = GlState::Failed;  // latch until proven ready

            if (!glewReady()) {
                log::warn("gpu batch cull: GL 4.3 entry points missing; falling back to the CPU filter");
                return false;
            }

            char const* version = reinterpret_cast<char const*>(pGetString(kGlVersion));
            int major = 0, minor = 0;
            if (!version || std::sscanf(version, "%d.%d", &major, &minor) != 2
                || major < 4 || (major == 4 && minor < 3)) {
                log::warn("gpu batch cull: GL context is older than 4.3; falling back to the CPU filter");
                return false;
            }

            if (!buildProgram()) return false;

            unsigned int buffers[2] = {0, 0};
            pGenBuffers(2, buffers);
            s_commandBuffer = buffers[0];
            s_indexBuffer = buffers[1];
            if (!s_commandBuffer || !s_indexBuffer) {
                log::warn("gpu batch cull: buffer allocation failed; falling back to the CPU filter");
                return false;
            }

            // Indirect command for glDrawElementsIndirect:
            // count, instanceCount, firstIndex, baseVertex, baseInstance.
            // count is rewritten by the compute shader each dispatch; the rest never changes.
            unsigned int const command[5] = {0, 1, 0, 0, 0};
            pBindBuffer(GL_ARRAY_BUFFER, s_commandBuffer);
            pBufferData(GL_ARRAY_BUFFER, static_cast<ptrdiff_t>(sizeof(command)), command, GL_STREAM_DRAW);
            pBindBuffer(GL_ARRAY_BUFFER, s_indexBuffer);
            pBufferData(GL_ARRAY_BUFFER, static_cast<ptrdiff_t>(kMaxGpuQuads * 6ull * 4ull), nullptr, GL_STREAM_DRAW);
            pBindBuffer(GL_ARRAY_BUFFER, 0);

            s_state = GlState::Ready;
            log::info("gpu batch cull: GL 4.3 compute path ready");
            return true;
        }

    }  // namespace

    bool gpuBatchCullAvailable() {
        return ensureReady();
    }

    std::uint64_t gpuCullBatchesSubmitted() {
        return s_batchesSubmitted.load(std::memory_order_relaxed);
    }

    std::uint64_t gpuCullQuadsScanned() {
        return s_quadsScanned.load(std::memory_order_relaxed);
    }

    GpuCullResult drawBatchGpuCull(cocos2d::CCTextureAtlas* atlas, unsigned int total, GLuint batchProgram) {
        if (!atlas || total == 0 || total > kMaxGpuQuads) return GpuCullResult::Unavailable;
        if (!ensureReady()) return GpuCullResult::Unavailable;

        kmMat4 projection{};
        kmMat4 modelview{};
        kmGLGetMatrix(KM_GL_PROJECTION, &projection);
        kmGLGetMatrix(KM_GL_MODELVIEW, &modelview);
        for (float const value : projection.mat) {
            if (!std::isfinite(value)) return GpuCullResult::Unavailable;
        }
        // Same gate as the CPU filter: only the stock screen-space 2D projection.
        if (std::fabs(projection.mat[3]) >= 1e-6f
            || std::fabs(projection.mat[7]) >= 1e-6f
            || std::fabs(projection.mat[11]) >= 1e-6f
            || std::fabs(projection.mat[15] - 1.f) >= 1e-6f) {
            return GpuCullResult::Unavailable;
        }
        kmMat4 mvp{};
        kmMat4Multiply(&mvp, &projection, &modelview);

        // Reset the indirect command's count; the compute shader rewrites it.
        unsigned int const zero = 0;
        pBindBuffer(GL_ARRAY_BUFFER, s_commandBuffer);
        pBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<ptrdiff_t>(sizeof(zero)), &zero);
        pBindBuffer(GL_ARRAY_BUFFER, 0);

        // The atlas vertex VBO is read directly as an SSBO - the same bytes the original
        // draw would upload, never a copy. The cmd buffer doubles as indirect command
        // storage and as the atomic counter for the index output.
        unsigned int const vbo = atlas->m_pBuffersVBO[0];
        pBindBufferBase(kGlShaderStorageBuffer, 0, vbo);
        pBindBufferBase(kGlShaderStorageBuffer, 1, s_commandBuffer);
        pBindBufferBase(kGlShaderStorageBuffer, 2, s_indexBuffer);

        pUseProgram(s_program);
        pUniformMatrix4fv(s_mvpLocation, 1, GL_FALSE, mvp.mat);
        pUniform1ui(s_totalLocation, total);
        unsigned int const groups = (total + kLocalSizeX - 1) / kLocalSizeX;
        pDispatchCompute(groups, 1, 1);
        pMemoryBarrier(kGlShaderStorageBarrierBit | kGlElementArrayBarrierBit | kGlCommandBarrierBit);

        // The batch's program (bound by the caller's CC_NODE_DRAW_SETUP) draws.
        pUseProgram(batchProgram);

        if (cocos2d::CCTexture2D* texture = atlas->getTexture()) {
            ccGLBindTexture2D(texture->getName());
        }

        reborn::setupAtlasVertexAttribs(atlas);

        pBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_indexBuffer);
        pBindBuffer(GL_DRAW_INDIRECT_BUFFER, s_commandBuffer);
        // Element indices are GL_UNSIGNED_INT; the command's count field is kept*6.
        pDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr);
        CC_INCREMENT_GL_DRAWS(1);

        pBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        pBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        pBindBufferBase(kGlShaderStorageBuffer, 0, 0);
        pBindBufferBase(kGlShaderStorageBuffer, 1, 0);
        pBindBufferBase(kGlShaderStorageBuffer, 2, 0);

        s_batchesSubmitted.fetch_add(1, std::memory_order_relaxed);
        s_quadsScanned.fetch_add(total, std::memory_order_relaxed);
        return GpuCullResult::Submitted;
    }

}  // namespace reborn

#else  // !GEODE_IS_WINDOWS

namespace reborn {
    bool gpuBatchCullAvailable() { return false; }
    std::uint64_t gpuCullBatchesSubmitted() { return 0; }
    std::uint64_t gpuCullQuadsScanned() { return 0; }
    GpuCullResult drawBatchGpuCull(cocos2d::CCTextureAtlas*, unsigned int, unsigned int) {
        return GpuCullResult::Unavailable;
    }
}

#endif
