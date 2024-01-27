#pragma once

#include <daxa/daxa.inl>
#include <daxa/utils/task_graph.inl>

#include "../../shader_shared/shared.inl"
#include "../../shader_shared/asset.inl"
#include "../../shader_shared/visbuffer.inl"
#include "../../shader_shared/scene.inl"

DAXA_DECL_TASK_HEAD_BEGIN(DrawVisbuffer_WriteCommand, 2)
DAXA_TH_BUFFER_PTR(COMPUTE_SHADER_READ, daxa_BufferPtr(MeshletInstances), instantiated_meshlets)
DAXA_TH_BUFFER_PTR(COMPUTE_SHADER_WRITE, daxa_u64, command)
DAXA_DECL_TASK_HEAD_END

// When drawing triangles, this draw command has triangle ids appended to the end of the command.
DAXA_DECL_TASK_HEAD_BEGIN(DrawVisbuffer, 7)
DAXA_TH_BUFFER_PTR(DRAW_INDIRECT_INFO_READ, daxa_u64, command)
DAXA_TH_BUFFER_PTR(GRAPHICS_SHADER_READ, daxa_BufferPtr(MeshletInstances), instantiated_meshlets)
DAXA_TH_BUFFER_PTR(GRAPHICS_SHADER_READ, daxa_BufferPtr(GPUMesh), meshes)
DAXA_TH_BUFFER_PTR(GRAPHICS_SHADER_READ, daxa_BufferPtr(daxa_f32mat4x3), entity_combined_transforms)
DAXA_TH_IMAGE(COLOR_ATTACHMENT, REGULAR_2D, vis_image)
DAXA_TH_IMAGE(COLOR_ATTACHMENT, REGULAR_2D, debug_image)
DAXA_TH_IMAGE(DEPTH_ATTACHMENT, REGULAR_2D, depth_image)
DAXA_DECL_TASK_HEAD_END

DAXA_DECL_TASK_HEAD_BEGIN(DrawVisbuffer_MeshShader, 14)
// When drawing triangles, this draw command has triangle ids appended to the end of the command.
DAXA_TH_BUFFER_PTR(DRAW_INDIRECT_INFO_READ, daxa_u64, command)
DAXA_TH_BUFFER_PTR(GRAPHICS_SHADER_READ, daxa_BufferPtr(MeshletCullIndirectArgTable), meshlet_cull_indirect_args)
DAXA_TH_BUFFER_PTR(GRAPHICS_SHADER_READ, daxa_BufferPtr(GPUMesh), meshes)
DAXA_TH_BUFFER_PTR(GRAPHICS_SHADER_READ, daxa_BufferPtr(GPUEntityMetaData), entity_meta)
DAXA_TH_BUFFER_PTR(GRAPHICS_SHADER_READ, daxa_BufferPtr(daxa_u32), entity_meshgroups)
DAXA_TH_BUFFER_PTR(GRAPHICS_SHADER_READ, daxa_BufferPtr(GPUMeshGroup), meshgroups)
DAXA_TH_BUFFER_PTR(GRAPHICS_SHADER_READ, daxa_BufferPtr(daxa_f32mat4x3), entity_combined_transforms)
DAXA_TH_BUFFER_PTR(GRAPHICS_SHADER_READ, EntityMeshletVisibilityBitfieldOffsetsView, entity_meshlet_visibility_bitfield_offsets)
DAXA_TH_BUFFER_PTR(GRAPHICS_SHADER_READ, daxa_BufferPtr(daxa_u32), entity_meshlet_visibility_bitfield_arena)
DAXA_TH_IMAGE_ID(GRAPHICS_SHADER_SAMPLED, REGULAR_2D, hiz)
DAXA_TH_BUFFER_PTR(GRAPHICS_SHADER_READ_WRITE, daxa_RWBufferPtr(MeshletInstances), instantiated_meshlets)
DAXA_TH_IMAGE(COLOR_ATTACHMENT, REGULAR_2D, vis_image)
DAXA_TH_IMAGE(COLOR_ATTACHMENT, REGULAR_2D, debug_image)
DAXA_TH_IMAGE(DEPTH_ATTACHMENT, REGULAR_2D, depth_image)
DAXA_DECL_TASK_HEAD_END

#define DRAW_VISBUFFER_PASS_ONE 0
#define DRAW_VISBUFFER_PASS_TWO 1
#define DRAW_VISBUFFER_PASS_OBSERVER 2

struct DrawVisbufferPush_WriteCommand
{
    daxa_BufferPtr(ShaderGlobals) globals;
    daxa_u32 pass;
    daxa_u32 mesh_shader;
    DAXA_TH_BLOB(DrawVisbuffer_WriteCommand, uses)
};

struct DrawVisbufferPush
{
    daxa_BufferPtr(ShaderGlobals) globals;
    daxa_u32 pass;
    DAXA_TH_BLOB(DrawVisbuffer, uses)
};

struct DrawVisbufferPush_MeshShader
{
    daxa_BufferPtr(ShaderGlobals) globals;
    daxa_u32 bucket_index;
    DAXA_TH_BLOB(DrawVisbuffer_MeshShader, uses)
};

#if __cplusplus
#include "../../gpu_context.hpp"
#include "../tasks/misc.hpp"
#include "cull_meshlets.inl"

static constexpr inline char const DRAW_VISBUFFER_SHADER_PATH[] = "./src/rendering/rasterize_visbuffer/draw_visbuffer.glsl";

static inline daxa::DepthTestInfo DRAW_VISBUFFER_DEPTH_TEST_INFO = {
    .depth_attachment_format = daxa::Format::D32_SFLOAT,
    .enable_depth_write = true,
    .depth_test_compare_op = daxa::CompareOp::GREATER,
    .min_depth_bounds = 0.0f,
    .max_depth_bounds = 1.0f,
};

static inline std::vector<daxa::RenderAttachment> DRAW_VISBUFFER_RENDER_ATTACHMENT_INFOS = {
    daxa::RenderAttachment{
        .format = daxa::Format::R32_UINT,
    },
    daxa::RenderAttachment{
        .format = daxa::Format::R16G16B16A16_SFLOAT,
    },
};

using DrawVisbuffer_WriteCommandTask =
    WriteIndirectDispatchArgsPushBaseTask<DrawVisbuffer_WriteCommand, DRAW_VISBUFFER_SHADER_PATH, DrawVisbufferPush_WriteCommand>;

inline static daxa::RasterPipelineCompileInfo const DRAW_VISBUFFER_PIPELINE_COMPILE_INFO_NO_MESH_SHADER = []()
{
    auto ret = daxa::RasterPipelineCompileInfo{};
    ret.depth_test = DRAW_VISBUFFER_DEPTH_TEST_INFO;
    ret.color_attachments = DRAW_VISBUFFER_RENDER_ATTACHMENT_INFOS;
    ret.fragment_shader_info = daxa::ShaderCompileInfo{
        .source = daxa::ShaderFile{DRAW_VISBUFFER_SHADER_PATH},
        .compile_options = {.defines = {{"NO_MESH_SHADER", "1"}}},
    };
    ret.vertex_shader_info = daxa::ShaderCompileInfo{
        .source = daxa::ShaderFile{DRAW_VISBUFFER_SHADER_PATH},
        .compile_options = {.defines = {{"NO_MESH_SHADER", "1"}}},
    };
    ret.name = "DrawVisbuffer";
    ret.push_constant_size = s_cast<u32>(sizeof(DrawVisbufferPush) + DrawVisbuffer::attachment_shader_data_size());
    return ret;
}();

inline static daxa::RasterPipelineCompileInfo const DRAW_VISBUFFER_PIPELINE_COMPILE_INFO_MESH_SHADER = []()
{
    auto ret = daxa::RasterPipelineCompileInfo{};
    ret.depth_test = DRAW_VISBUFFER_DEPTH_TEST_INFO;
    ret.color_attachments = DRAW_VISBUFFER_RENDER_ATTACHMENT_INFOS;
    ret.fragment_shader_info = daxa::ShaderCompileInfo{
        .source = daxa::ShaderFile{DRAW_VISBUFFER_SHADER_PATH},
        .compile_options = {.defines = {{"MESH_SHADER", "1"}}},
    };
    ret.mesh_shader_info = daxa::ShaderCompileInfo{
        .source = daxa::ShaderFile{DRAW_VISBUFFER_SHADER_PATH},
        .compile_options = {.defines = {{"MESH_SHADER", "1"}}},
    };
    ret.name = "DrawVisbufferMeshShader";
    // TODO(msakmary + pahrens) I have a very strong suspicion this is broken - why is mesh shader pipeline using the Draw Visbuffer push constant
    // and not a Mesh shader version of the push constant???
    ret.push_constant_size = s_cast<u32>(sizeof(DrawVisbufferPush) + DrawVisbuffer::attachment_shader_data_size());
    return ret;
}();

inline static daxa::RasterPipelineCompileInfo const DRAW_VISBUFFER_PIPELINE_COMPILE_INFO_MESH_SHADER_CULL_AND_DRAW = []()
{
    auto ret = daxa::RasterPipelineCompileInfo{};
    ret.depth_test = DRAW_VISBUFFER_DEPTH_TEST_INFO;
    ret.color_attachments = DRAW_VISBUFFER_RENDER_ATTACHMENT_INFOS;
    auto comp_info = daxa::ShaderCompileInfo{
        .source = daxa::ShaderFile{DRAW_VISBUFFER_SHADER_PATH},
        .compile_options = {.defines = {{"MESH_SHADER_CULL_AND_DRAW", "1"}}},
    };
    ret.fragment_shader_info = comp_info;
    ret.mesh_shader_info = comp_info;
    ret.task_shader_info = comp_info;
    ret.name = "DrawVisbuffer_MeshShader";
    ret.push_constant_size = s_cast<u32>(sizeof(DrawVisbufferPush_MeshShader) + DrawVisbuffer_MeshShader::attachment_shader_data_size());
    return ret;
}();

struct DrawVisbufferTask : DrawVisbuffer
{
    DrawVisbuffer::Views views = {};
    inline static daxa::RasterPipelineCompileInfo const PIPELINE_COMPILE_INFO =
        DRAW_VISBUFFER_PIPELINE_COMPILE_INFO_NO_MESH_SHADER;
    GPUContext * context = {};
    u32 pass = {};
    bool mesh_shader = {};
    void callback(daxa::TaskInterface ti)
    {
        bool const clear_images = pass == DRAW_VISBUFFER_PASS_ONE || pass == DRAW_VISBUFFER_PASS_OBSERVER;
        auto [x, y, z] = ti.device.info_image(ti.get(depth_image).ids[0]).value().size;
        auto load_op = clear_images ? daxa::AttachmentLoadOp::CLEAR : daxa::AttachmentLoadOp::LOAD;
        daxa::RenderPassBeginInfo render_pass_begin_info{
            .depth_attachment =
                daxa::RenderAttachmentInfo{
                    .image_view = ti.get(depth_image).ids[0].default_view(),
                    .layout = daxa::ImageLayout::ATTACHMENT_OPTIMAL,
                    .load_op = load_op,
                    .store_op = daxa::AttachmentStoreOp::STORE,
                    .clear_value = daxa::DepthValue{0.0f, 0},
                },
            .render_area = daxa::Rect2D{.width = x, .height = y},
        };
        render_pass_begin_info.color_attachments = {
            daxa::RenderAttachmentInfo{
                .image_view = ti.get(vis_image).ids[0].default_view(),
                .layout = daxa::ImageLayout::ATTACHMENT_OPTIMAL,
                .load_op = load_op,
                .store_op = daxa::AttachmentStoreOp::STORE,
                .clear_value = std::array<u32, 4>{INVALID_TRIANGLE_ID, 0, 0, 0},
            },
            daxa::RenderAttachmentInfo{
                .image_view = ti.get(debug_image).ids[0].default_view(),
                .layout = daxa::ImageLayout::ATTACHMENT_OPTIMAL,
                .load_op = load_op,
                .store_op = daxa::AttachmentStoreOp::STORE,
                .clear_value = daxa::ClearValue{std::array<u32, 4>{0, 0, 0, 0}},
            },
        };
        auto render_cmd = std::move(ti.recorder).begin_renderpass(render_pass_begin_info);
        if (mesh_shader)
        {
            render_cmd.set_pipeline(*context->raster_pipelines.at(DRAW_VISBUFFER_PIPELINE_COMPILE_INFO_MESH_SHADER.name));
        }
        else
        {
            render_cmd.set_pipeline(*context->raster_pipelines.at(PIPELINE_COMPILE_INFO.name));
        }

        render_cmd.push_constant(DrawVisbufferPush{
            .globals = context->shader_globals_address,
            .pass = pass,
        });
        render_cmd.push_constant_vptr({
            .data = ti.attachment_shader_data.data(),
            .size = ti.attachment_shader_data.size(),
            .offset = sizeof(DrawVisbufferPush),
        });
        if (mesh_shader)
        {
            render_cmd.draw_mesh_tasks_indirect({
                .indirect_buffer = ti.get(command).ids[0],
                .offset = 0,
                .draw_count = 1,
                .stride = sizeof(DispatchIndirectStruct),
            });
        }
        else
        {
            render_cmd.draw_indirect({
                .draw_command_buffer = ti.get(command).ids[0],
                .draw_count = 1,
                .draw_command_stride = sizeof(DrawIndirectStruct),
            });
        }
        ti.recorder = std::move(render_cmd).end_renderpass();
    }
};

struct CullAndDrawVisbufferTask : DrawVisbuffer_MeshShader
{
    DrawVisbuffer_MeshShader::Views views = {};
    inline static daxa::RasterPipelineCompileInfo const PIPELINE_COMPILE_INFO =
        DRAW_VISBUFFER_PIPELINE_COMPILE_INFO_MESH_SHADER_CULL_AND_DRAW;
    GPUContext * context = {};
    void callback(daxa::TaskInterface ti)
    {
        bool const clear_images = false;
        auto load_op = clear_images ? daxa::AttachmentLoadOp::CLEAR : daxa::AttachmentLoadOp::LOAD;
        auto [x, y, z] = ti.device.info_image(ti.get(depth_image).ids[0]).value().size;
        daxa::RenderPassBeginInfo render_pass_begin_info{
            .depth_attachment =
                daxa::RenderAttachmentInfo{
                    .image_view = ti.get(depth_image).ids[0].default_view(),
                    .layout = daxa::ImageLayout::ATTACHMENT_OPTIMAL,
                    .load_op = load_op,
                    .store_op = daxa::AttachmentStoreOp::STORE,
                    .clear_value = daxa::ClearValue{daxa::DepthValue{0.0f, 0}},
                },
            .render_area = daxa::Rect2D{.width = x, .height = y},
        };
        render_pass_begin_info.color_attachments = {
            daxa::RenderAttachmentInfo{
                .image_view = ti.get(vis_image).ids[0].default_view(),
                .layout = daxa::ImageLayout::ATTACHMENT_OPTIMAL,
                .load_op = load_op,
                .store_op = daxa::AttachmentStoreOp::STORE,
                .clear_value = daxa::ClearValue{std::array<u32, 4>{INVALID_TRIANGLE_ID, 0, 0, 0}},
            },
            daxa::RenderAttachmentInfo{
                .image_view = ti.get(debug_image).ids[0].default_view(),
                .layout = daxa::ImageLayout::ATTACHMENT_OPTIMAL,
                .load_op = load_op,
                .store_op = daxa::AttachmentStoreOp::STORE,
                .clear_value = daxa::ClearValue{std::array<u32, 4>{0, 0, 0, 0}},
            },
        };
        auto render_cmd = std::move(ti.recorder).begin_renderpass(render_pass_begin_info);
        render_cmd.set_pipeline(
            *context->raster_pipelines.at(DRAW_VISBUFFER_PIPELINE_COMPILE_INFO_MESH_SHADER_CULL_AND_DRAW.name));
        for (u32 i = 0; i < 32; ++i)
        {
            render_cmd.push_constant(DrawVisbufferPush_MeshShader{
                .globals = context->shader_globals_address,
                .bucket_index = i,
            });
            render_cmd.push_constant_vptr({
                .data = ti.attachment_shader_data.data(),
                .size = ti.attachment_shader_data.size(),
                .offset = sizeof(DrawVisbufferPush_MeshShader),
            });
            render_cmd.draw_mesh_tasks_indirect({
                .indirect_buffer = ti.get(command).ids[0],
                .offset = sizeof(DispatchIndirectStruct) * i,
                .draw_count = 1,
                .stride = sizeof(DispatchIndirectStruct),
            });
        }
        ti.recorder = std::move(render_cmd).end_renderpass();
    }
};

struct TaskCullAndDrawVisbufferInfo
{
    GPUContext * context = {};
    daxa::TaskGraph & tg;
    bool const enable_mesh_shader = {};
    daxa::TaskBufferView cull_meshlets_commands = {};
    daxa::TaskBufferView meshlet_cull_indirect_args = {};
    daxa::TaskBufferView entity_meta_data = {};
    daxa::TaskBufferView entity_meshgroups = {};
    daxa::TaskBufferView entity_combined_transforms = {};
    daxa::TaskBufferView mesh_groups = {};
    daxa::TaskBufferView meshes = {};
    daxa::TaskBufferView entity_meshlet_visibility_bitfield_offsets = {};
    daxa::TaskBufferView entity_meshlet_visibility_bitfield_arena = {};
    daxa::TaskImageView hiz = {};
    daxa::TaskBufferView meshlet_instances = {};
    daxa::TaskImageView vis_image = {};
    daxa::TaskImageView debug_image = {};
    daxa::TaskImageView depth_image = {};
};
inline void task_cull_and_draw_visbuffer(TaskCullAndDrawVisbufferInfo const & info)
{
    if (info.enable_mesh_shader)
    {
        info.tg.add_task(CullAndDrawVisbufferTask{
            .views = std::array{
                daxa::TaskViewVariant{std::pair{CullAndDrawVisbufferTask::command, info.cull_meshlets_commands}},
                daxa::TaskViewVariant{std::pair{CullAndDrawVisbufferTask::meshlet_cull_indirect_args, info.meshlet_cull_indirect_args}},
                daxa::TaskViewVariant{std::pair{CullAndDrawVisbufferTask::instantiated_meshlets, info.meshlet_instances}},
                daxa::TaskViewVariant{std::pair{CullAndDrawVisbufferTask::meshes, info.meshes}},
                daxa::TaskViewVariant{std::pair{CullAndDrawVisbufferTask::entity_meta, info.entity_meta_data}},
                daxa::TaskViewVariant{std::pair{CullAndDrawVisbufferTask::entity_meshgroups, info.entity_meshgroups}},
                daxa::TaskViewVariant{std::pair{CullAndDrawVisbufferTask::meshgroups, info.mesh_groups}},
                daxa::TaskViewVariant{std::pair{CullAndDrawVisbufferTask::entity_combined_transforms, info.entity_combined_transforms}},
                daxa::TaskViewVariant{std::pair{CullAndDrawVisbufferTask::entity_meshlet_visibility_bitfield_offsets, info.entity_meshlet_visibility_bitfield_offsets}},
                daxa::TaskViewVariant{std::pair{CullAndDrawVisbufferTask::entity_meshlet_visibility_bitfield_arena, info.entity_meshlet_visibility_bitfield_arena}},
                daxa::TaskViewVariant{std::pair{CullAndDrawVisbufferTask::hiz, info.hiz}},
                daxa::TaskViewVariant{std::pair{CullAndDrawVisbufferTask::vis_image, info.vis_image}},
                daxa::TaskViewVariant{std::pair{CullAndDrawVisbufferTask::debug_image, info.debug_image}},
                daxa::TaskViewVariant{std::pair{CullAndDrawVisbufferTask::depth_image, info.depth_image}},
            },
            .context = info.context,
        });
    }
    else
    {
        auto draw_command = info.tg.create_transient_buffer({
            .size = static_cast<u32>(std::max(sizeof(DrawIndirectStruct), sizeof(DispatchIndirectStruct))),
            .name = std::string("draw visbuffer command buffer") + info.context->dummy_string(),
        });
        // clear to zero, rest of values will be initialized by CullMeshletsTask.
        task_clear_buffer(info.tg, draw_command, 0);
        CullMeshletsTask cull_task = {
            .views = std::array{
                daxa::TaskViewVariant{std::pair{CullMeshletsTask::hiz, info.hiz}},
                daxa::TaskViewVariant{std::pair{CullMeshletsTask::commands, info.cull_meshlets_commands}},
                daxa::TaskViewVariant{std::pair{CullMeshletsTask::meshlet_cull_indirect_args, info.meshlet_cull_indirect_args}},
                daxa::TaskViewVariant{std::pair{CullMeshletsTask::entity_meta_data, info.entity_meta_data}},
                daxa::TaskViewVariant{std::pair{CullMeshletsTask::entity_meshgroups, info.entity_meshgroups}},
                daxa::TaskViewVariant{std::pair{CullMeshletsTask::meshgroups, info.mesh_groups}},
                daxa::TaskViewVariant{std::pair{CullMeshletsTask::entity_combined_transforms, info.entity_combined_transforms}},
                daxa::TaskViewVariant{std::pair{CullMeshletsTask::meshes, info.meshes}},
                daxa::TaskViewVariant{std::pair{CullMeshletsTask::entity_meshlet_visibility_bitfield_offsets, info.entity_meshlet_visibility_bitfield_offsets}},
                daxa::TaskViewVariant{std::pair{CullMeshletsTask::entity_meshlet_visibility_bitfield_arena, info.entity_meshlet_visibility_bitfield_arena}},
                daxa::TaskViewVariant{std::pair{CullMeshletsTask::instantiated_meshlets, info.meshlet_instances}},
                daxa::TaskViewVariant{std::pair{CullMeshletsTask::draw_command, draw_command}},
            },
            .context = info.context,
        };
        info.tg.add_task(cull_task);

        DrawVisbufferTask draw_task = {
            .views = std::array{
                daxa::TaskViewVariant{std::pair{DrawVisbufferTask::command, draw_command}},
                daxa::TaskViewVariant{std::pair{DrawVisbufferTask::instantiated_meshlets, info.meshlet_instances}},
                daxa::TaskViewVariant{std::pair{DrawVisbufferTask::meshes, info.meshes}},
                daxa::TaskViewVariant{std::pair{DrawVisbufferTask::entity_combined_transforms, info.entity_combined_transforms}},
                daxa::TaskViewVariant{std::pair{DrawVisbufferTask::vis_image, info.vis_image}},
                daxa::TaskViewVariant{std::pair{DrawVisbufferTask::debug_image, info.debug_image}},
                daxa::TaskViewVariant{std::pair{DrawVisbufferTask::depth_image, info.depth_image}},
            },
            .context = info.context,
            .pass = DRAW_VISBUFFER_PASS_TWO,
            .mesh_shader = false,
        };
        info.tg.add_task(draw_task);
    }
}

struct TaskDrawVisbufferInfo
{
    GPUContext * context = {};
    daxa::TaskGraph & tg;
    bool const enable_mesh_shader = {};
    u32 const pass = {};
    daxa::TaskBufferView meshlet_instances = {};
    daxa::TaskBufferView meshes = {};
    daxa::TaskBufferView combined_transforms = {};
    daxa::TaskImageView vis_image = {};
    daxa::TaskImageView debug_image = {};
    daxa::TaskImageView depth_image = {};
};

inline void task_draw_visbuffer(TaskDrawVisbufferInfo const & info)
{
    auto draw_command = info.tg.create_transient_buffer({
        .size = static_cast<u32>(std::max(sizeof(DrawIndirectStruct), sizeof(DispatchIndirectStruct))),
        .name = std::string("draw visbuffer command buffer") + info.context->dummy_string(),
    });

    DrawVisbuffer_WriteCommandTask write_task = {
        .views = std::array{
            daxa::TaskViewVariant{std::pair{DrawVisbuffer_WriteCommandTask::instantiated_meshlets, info.meshlet_instances}},
            daxa::TaskViewVariant{std::pair{DrawVisbuffer_WriteCommandTask::command, draw_command}},
        },
        .context = info.context,
        .push = DrawVisbufferPush_WriteCommand{.pass = info.pass, .mesh_shader = info.enable_mesh_shader ? 1u : 0u},
    };
    info.tg.add_task(write_task);

    DrawVisbufferTask draw_task = {
        .views = std::array{
            daxa::TaskViewVariant{std::pair{DrawVisbufferTask::command, draw_command}},
            daxa::TaskViewVariant{std::pair{DrawVisbufferTask::instantiated_meshlets, info.meshlet_instances}},
            daxa::TaskViewVariant{std::pair{DrawVisbufferTask::meshes, info.meshes}},
            daxa::TaskViewVariant{std::pair{DrawVisbufferTask::entity_combined_transforms, info.combined_transforms}},
            daxa::TaskViewVariant{std::pair{DrawVisbufferTask::vis_image, info.vis_image}},
            daxa::TaskViewVariant{std::pair{DrawVisbufferTask::debug_image, info.debug_image}},
            daxa::TaskViewVariant{std::pair{DrawVisbufferTask::depth_image, info.depth_image}},
        },
        .context = info.context,
        .pass = info.pass,
        .mesh_shader = info.enable_mesh_shader,
    };
    info.tg.add_task(draw_task);
}
#endif