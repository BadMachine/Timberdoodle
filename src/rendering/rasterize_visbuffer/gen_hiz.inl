#pragma once

#include <daxa/daxa.inl>
#include <daxa/utils/task_graph.inl>

#include "../../shader_shared/shared.inl"
#include "../../shader_shared/globals.inl"
#include "../../shader_shared/geometry.inl"

#define GEN_HIZ_X 16
#define GEN_HIZ_Y 16
#define GEN_HIZ_LEVELS_PER_DISPATCH 12
#define GEN_HIZ_WINDOW_X 64
#define GEN_HIZ_WINDOW_Y 64

DAXA_DECL_TASK_HEAD_BEGIN(GenHizTH)
DAXA_TH_BUFFER_PTR(COMPUTE_SHADER_READ_WRITE_CONCURRENT, daxa_BufferPtr(RenderGlobalData), globals)
DAXA_TH_IMAGE_ID(COMPUTE_SHADER_SAMPLED, REGULAR_2D, src)
DAXA_TH_IMAGE_ID_MIP_ARRAY(COMPUTE_SHADER_STORAGE_READ_WRITE, REGULAR_2D, mips, GEN_HIZ_LEVELS_PER_DISPATCH)
DAXA_DECL_TASK_HEAD_END

// DAXA_DECL_TASK_HEAD_BEGIN(GenHiz2TH)
// DAXA_TH_BUFFER_PTR(COMPUTE_SHADER_READ_WRITE_CONCURRENT, daxa_BufferPtr(RenderGlobalData), globals)
// DAXA_TH_IMAGE_TYPED(COMPUTE_SHADER_SAMPLED, daxa::Texture2DId<float>, src)
// // Use index here to save on push constant space
// DAXA_TH_IMAGE_TYPED_MIP_ARRAY(COMPUTE_SHADER_STORAGE_READ_WRITE, daxa::RWTexture2DIndex<float>, mips, GEN_HIZ_LEVELS_PER_DISPATCH)
// DAXA_DECL_TASK_HEAD_END

struct GenHizPush
{
    DAXA_TH_BLOB(GenHizTH, uses)
    daxa_RWBufferPtr(daxa_u32) counter;
    daxa_u32 mip_count;
    daxa_u32 total_workgroup_count;
};

#if (!DAXA_SHADER_LANG == DAXA_SHADERLANG_GLSL)
struct GenHizInfo
{
    daxa_u32vec2 src_image_resolution;
    daxa_f32vec2 src_image_po2_resolution;
    daxa_u32vec2 inv_src_image_resolution;
    daxa_f32vec2 inv_src_image_po2_resolution;
};

// The po2 resolutions are the next higher power of two resolution of the src images original resolution.
// For images not power of two, we simply oversample the original image to the next power of two size.
// This also means we dispatch enough threads for the size of the po2, not the original image size. 
// struct GenHizPush2
// {
//     GenHiz2TH::AttachmentShaderBlob at;
//     daxa_RWBufferPtr(daxa_u32) counter;
//     GenHizInfo info;
// };
#endif

#if defined(__cplusplus)

#include <format>
#include "../../gpu_context.hpp"
#include "../scene_renderer_context.hpp"

inline daxa::ComputePipelineCompileInfo gen_hiz_pipeline_compile_info()
{
    return {
        .shader_info = daxa::ShaderCompileInfo{daxa::ShaderFile{"./src/rendering/rasterize_visbuffer/gen_hiz.glsl"}},
        .push_constant_size = s_cast<u32>(sizeof(GenHizPush)),
        .name = std::string{"GenHiz"},
    };
};

// inline daxa::ComputePipelineCompileInfo gen_hiz_pipeline_compile_info2()
// {
//     return {
//         .shader_info = daxa::ShaderCompileInfo{
//             .source = daxa::ShaderFile{"./src/rendering/rasterize_visbuffer/gen_hiz.hlsl"},
//             .compile_options = {
//                 .entry_point = "entry_gen_hiz",
//                 .language = daxa::ShaderLanguage::SLANG,
//             },
//         },
//         .push_constant_size = s_cast<u32>(sizeof(GenHizPush2)),
//         .name = std::string{"GenHiz"},
//     };
// };

struct GenHizTask : GenHizTH::Task
{
    AttachmentViews views = {};
    RenderContext * render_context = {};
    void callback(daxa::TaskInterface ti)
    {
        ti.recorder.set_pipeline(*render_context->gpuctx->compute_pipelines.at(gen_hiz_pipeline_compile_info().name));
        daxa_u32vec2 next_higher_po2_render_target_size = {
            render_context->render_data.settings.next_lower_po2_render_target_size.x,
            render_context->render_data.settings.next_lower_po2_render_target_size.y,
        };
        auto const dispatch_x = round_up_div(next_higher_po2_render_target_size.x * 2, GEN_HIZ_WINDOW_X);
        auto const dispatch_y = round_up_div(next_higher_po2_render_target_size.y * 2, GEN_HIZ_WINDOW_Y);
        GenHizPush push = {
            .counter = ti.allocator->allocate_fill(0u).value().device_address,
            .mip_count = ti.get(AT.mips).view.slice.level_count,
            .total_workgroup_count = dispatch_x * dispatch_y,
        };
        assign_blob(push.uses, ti.attachment_shader_blob);
        ti.recorder.push_constant(push);
        ti.recorder.dispatch({.x = dispatch_x, .y = dispatch_y, .z = 1});
    }
};

// struct GenHizTask2 : GenHiz2TH::Task
// {
//     AttachmentViews views = {};
//     RenderContext * render_context = {};
//     void callback(daxa::TaskInterface ti)
//     {
//         ti.recorder.set_pipeline(*render_context->gpuctx->compute_pipelines.at(gen_hiz_pipeline_compile_info2().name));
//         daxa_u32vec2 next_higher_po2_render_target_size = {
//             render_context->render_data.settings.next_lower_po2_render_target_size.x,
//             render_context->render_data.settings.next_lower_po2_render_target_size.y,
//         };
//         auto const dispatch_x = round_up_div(next_higher_po2_render_target_size.x * 2, GEN_HIZ_WINDOW_X);
//         auto const dispatch_y = round_up_div(next_higher_po2_render_target_size.y * 2, GEN_HIZ_WINDOW_Y);
//         GenHizPush2 push = {
//             .
//         };
//         assign_blob(push.uses, ti.attachment_shader_blob);
//         ti.recorder.push_constant(push);
//         ti.recorder.dispatch({.x = dispatch_x, .y = dispatch_y, .z = 1});
//     }
// };

struct TaskGenHizSinglePassInfo
{
    RenderContext * render_context = {};
    daxa::TaskGraph & task_graph;
    daxa::TaskImageView src = {};
    daxa::TaskBufferView globals = {};
    daxa::TaskImageView * hiz = {};
};
void task_gen_hiz_single_pass(TaskGenHizSinglePassInfo const & info)
{
    // daxa_u32vec2 const hiz_size =
    //     daxa_u32vec2(info.render_context->render_data.settings.render_target_size.x / 2, 
    //     info.render_context->render_data.settings.render_target_size.y / 2);
    daxa_u32vec2 const hiz_size =
        daxa_u32vec2(info.render_context->render_data.settings.next_lower_po2_render_target_size.x, 
        info.render_context->render_data.settings.next_lower_po2_render_target_size.y);
    daxa_u32 mip_count = static_cast<daxa_u32>(std::ceil(std::log2(std::max(hiz_size.x, hiz_size.y))));
    mip_count = std::min(mip_count, u32(GEN_HIZ_LEVELS_PER_DISPATCH));
    *info.hiz = info.task_graph.create_transient_image({
        .format = daxa::Format::R32_SFLOAT,
        .size = {hiz_size.x, hiz_size.y, 1},
        .mip_level_count = mip_count,
        .array_layer_count = 1,
        .sample_count = 1,
        .name = "hiz",
    });
    info.task_graph.add_task(GenHizTask{
        .views = std::array{
            daxa::attachment_view(GenHizTH::AT.globals, info.globals),
            daxa::attachment_view(GenHizTH::AT.src, info.src),
            daxa::attachment_view(GenHizTH::AT.mips, *info.hiz),
        },
        .render_context = info.render_context,
    });
}

#endif