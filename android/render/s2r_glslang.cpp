// GLSL -> SPIR-V on the device, for the shaders the build-time library does not
// have (a mod, another edition): s2r_shaderlib.c translates their bytecode
// with s2r_translate.c and the Vulkan backend compiles the text here. glslang
// 11.11 of the pinned NDK (its shaderc sources); the library's own shaders
// never come this way. The resource limits are generous defaults (generated
// from glslang/Include/ResourceLimits.h): they only bound what a shader may
// declare.
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static TBuiltInResource default_resources()
{
    TBuiltInResource r;
    std::memset(&r, 0, sizeof r);
    r.maxLights = 32;
    r.maxClipPlanes = 6;
    r.maxTextureUnits = 32;
    r.maxTextureCoords = 32;
    r.maxVertexAttribs = 64;
    r.maxVertexUniformComponents = 256;
    r.maxVaryingFloats = 128;
    r.maxVertexTextureImageUnits = 32;
    r.maxCombinedTextureImageUnits = 80;
    r.maxTextureImageUnits = 32;
    r.maxFragmentUniformComponents = 256;
    r.maxDrawBuffers = 32;
    r.maxVertexUniformVectors = 4096;
    r.maxVaryingVectors = 32;
    r.maxFragmentUniformVectors = 4096;
    r.maxVertexOutputVectors = 256;
    r.maxFragmentInputVectors = 256;
    r.minProgramTexelOffset = -8;
    r.maxProgramTexelOffset = 7;
    r.maxClipDistances = 8;
    r.maxComputeWorkGroupCountX = 256;
    r.maxComputeWorkGroupCountY = 256;
    r.maxComputeWorkGroupCountZ = 256;
    r.maxComputeWorkGroupSizeX = 256;
    r.maxComputeWorkGroupSizeY = 256;
    r.maxComputeWorkGroupSizeZ = 256;
    r.maxComputeUniformComponents = 256;
    r.maxComputeTextureImageUnits = 256;
    r.maxComputeImageUniforms = 256;
    r.maxComputeAtomicCounters = 256;
    r.maxComputeAtomicCounterBuffers = 256;
    r.maxVaryingComponents = 128;
    r.maxVertexOutputComponents = 256;
    r.maxGeometryInputComponents = 256;
    r.maxGeometryOutputComponents = 256;
    r.maxFragmentInputComponents = 256;
    r.maxImageUnits = 256;
    r.maxCombinedImageUnitsAndFragmentOutputs = 256;
    r.maxCombinedShaderOutputResources = 256;
    r.maxImageSamples = 256;
    r.maxVertexImageUniforms = 256;
    r.maxTessControlImageUniforms = 256;
    r.maxTessEvaluationImageUniforms = 256;
    r.maxGeometryImageUniforms = 256;
    r.maxFragmentImageUniforms = 256;
    r.maxCombinedImageUniforms = 256;
    r.maxGeometryTextureImageUnits = 256;
    r.maxGeometryOutputVertices = 256;
    r.maxGeometryTotalOutputComponents = 256;
    r.maxGeometryUniformComponents = 256;
    r.maxGeometryVaryingComponents = 256;
    r.maxTessControlInputComponents = 256;
    r.maxTessControlOutputComponents = 256;
    r.maxTessControlTextureImageUnits = 256;
    r.maxTessControlUniformComponents = 256;
    r.maxTessControlTotalOutputComponents = 256;
    r.maxTessEvaluationInputComponents = 256;
    r.maxTessEvaluationOutputComponents = 256;
    r.maxTessEvaluationTextureImageUnits = 256;
    r.maxTessEvaluationUniformComponents = 256;
    r.maxTessPatchComponents = 256;
    r.maxPatchVertices = 256;
    r.maxTessGenLevel = 256;
    r.maxViewports = 16;
    r.maxVertexAtomicCounters = 256;
    r.maxTessControlAtomicCounters = 256;
    r.maxTessEvaluationAtomicCounters = 256;
    r.maxGeometryAtomicCounters = 256;
    r.maxFragmentAtomicCounters = 256;
    r.maxCombinedAtomicCounters = 256;
    r.maxAtomicCounterBindings = 256;
    r.maxVertexAtomicCounterBuffers = 256;
    r.maxTessControlAtomicCounterBuffers = 256;
    r.maxTessEvaluationAtomicCounterBuffers = 256;
    r.maxGeometryAtomicCounterBuffers = 256;
    r.maxFragmentAtomicCounterBuffers = 256;
    r.maxCombinedAtomicCounterBuffers = 256;
    r.maxAtomicCounterBufferSize = 256;
    r.maxTransformFeedbackBuffers = 256;
    r.maxTransformFeedbackInterleavedComponents = 256;
    r.maxCullDistances = 8;
    r.maxCombinedClipAndCullDistances = 8;
    r.maxSamples = 4;
    r.maxMeshOutputVerticesNV = 256;
    r.maxMeshOutputPrimitivesNV = 256;
    r.maxMeshWorkGroupSizeX_NV = 32;
    r.maxMeshWorkGroupSizeY_NV = 256;
    r.maxMeshWorkGroupSizeZ_NV = 256;
    r.maxTaskWorkGroupSizeX_NV = 32;
    r.maxTaskWorkGroupSizeY_NV = 256;
    r.maxTaskWorkGroupSizeZ_NV = 256;
    r.maxMeshViewCountNV = 256;
    r.maxMeshOutputVerticesEXT = 256;
    r.maxMeshOutputPrimitivesEXT = 256;
    r.maxMeshWorkGroupSizeX_EXT = 128;
    r.maxMeshWorkGroupSizeY_EXT = 256;
    r.maxMeshWorkGroupSizeZ_EXT = 256;
    r.maxTaskWorkGroupSizeX_EXT = 128;
    r.maxTaskWorkGroupSizeY_EXT = 256;
    r.maxTaskWorkGroupSizeZ_EXT = 256;
    r.maxMeshViewCountEXT = 256;
    r.maxDualSourceDrawBuffersEXT = 256;
    r.limits.nonInductiveForLoops = true;
    r.limits.whileLoops = true;
    r.limits.doWhileLoops = true;
    r.limits.generalUniformIndexing = true;
    r.limits.generalAttributeMatrixVectorIndexing = true;
    r.limits.generalVaryingIndexing = true;
    r.limits.generalSamplerIndexing = true;
    r.limits.generalVariableIndexing = true;
    r.limits.generalConstantMatrixVectorIndexing = true;
    return r;
}

static void copy_log(char *log, size_t cap, const char *a, const char *b)
{
    if (!log || !cap) return;
    std::snprintf(log, cap, "%s%s", a ? a : "", b ? b : "");
}

extern "C" int s2r_glslang_compile(int pixel, const char *source, uint32_t **words, uint32_t *count, char *log, size_t log_cap)
{
    static bool ready = glslang::InitializeProcess();
    if (!ready) { copy_log(log, log_cap, "glslang did not initialise", nullptr); return 0; }
    static const TBuiltInResource resources = default_resources();
    EShLanguage stage = pixel ? EShLangFragment : EShLangVertex;
    glslang::TShader shader(stage);
    shader.setStrings(&source, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    /* Vulkan 1.0 / SPIR-V 1.0: every driver the renderer accepts reads it (tools/build_shaderlib.py matches). */
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(&resources, 450, false, messages)) {
        copy_log(log, log_cap, shader.getInfoLog(), shader.getInfoDebugLog());
        return 0;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        copy_log(log, log_cap, program.getInfoLog(), program.getInfoDebugLog());
        return 0;
    }
    std::vector<unsigned int> spirv;
    glslang::SpvOptions options;
    options.disableOptimizer = true;
    glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &options);
    if (spirv.empty()) { copy_log(log, log_cap, "no SPIR-V", nullptr); return 0; }
    *words = static_cast<uint32_t *>(std::malloc(spirv.size() * 4));
    if (!*words) return 0;
    std::memcpy(*words, spirv.data(), spirv.size() * 4);
    *count = static_cast<uint32_t>(spirv.size());
    return 1;
}
