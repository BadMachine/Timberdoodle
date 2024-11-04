#define DAXA_RAY_TRACING 1
#include <daxa/daxa.inl>
#include "shade_opaque.inl"

#include "shader_lib/visbuffer.glsl"
#include "shader_lib/debug.glsl"
#include "shader_lib/transform.glsl"
#include "shader_lib/depth_util.glsl"
#include "shader_lib/sky_util.glsl"
#include "shader_lib/vsm_util.glsl"
#include "shader_lib/misc.hlsl"
#include "shader_lib/volumetric.hlsl"


[[vk::push_constant]] ShadeOpaquePush push_opaque;

#define AT deref(push_opaque.attachments).attachments

float compute_exposure(float average_luminance) 
{
    const float exposure_bias = AT.globals->postprocess_settings.exposure_bias;
    const float calibration = AT.globals->postprocess_settings.calibration;
    const float sensor_sensitivity = AT.globals->postprocess_settings.exposure_bias;
    const float ev100 = log2(average_luminance * sensor_sensitivity * exposure_bias / calibration);
	const float exposure = 1.0 / (1.2 * exp2(ev100));
	return exposure;
}

static const uint PCF_NUM_SAMPLES = 1;
// https://developer.download.nvidia.com/whitepapers/2008/PCSS_Integration.pdf
static const float2 poisson_disk[16] = {
    float2( -0.94201624, -0.39906216 ),
    float2( 0.94558609, -0.76890725 ),
    float2( -0.094184101, -0.92938870 ),
    float2( 0.34495938, 0.29387760 ),
    float2( -0.91588581, 0.45771432 ),
    float2( -0.81544232, -0.87912464 ),
    float2( -0.38277543, 0.27676845 ),
    float2( 0.97484398, 0.75648379 ),
    float2( 0.44323325, -0.97511554 ),
    float2( 0.53742981, -0.47373420 ),
    float2( -0.26496911, -0.41893023 ),
    float2( 0.79197514, 0.19090188 ),
    float2( -0.24188840, 0.99706507 ),
    float2( -0.81409955, 0.91437590 ),
    float2( 0.19984126, 0.78641367 ),
    float2( 0.14383161, -0.14100790 )
};

struct AtmosphereLightingInfo
{
    // illuminance from atmosphere along normal vector
    float3 atmosphere_normal_illuminance;
    // illuminance from atmosphere along view vector
    float3 atmosphere_direct_illuminance;
    // direct sun illuminance
    float3 sun_direct_illuminance;
};

float3 get_sun_direct_lighting(daxa_BufferPtr(SkySettings) settings, float3 view_direction, float3 world_position)
{
    const float bottom_atmosphere_intersection_distance = ray_sphere_intersect_nearest(
        float3(0.0, 0.0, length(world_position)),
        view_direction,
        float3(0.0),
        settings->atmosphere_bottom
    );
    bool view_ray_intersects_ground = bottom_atmosphere_intersection_distance >= 0.0;
    const float3 direct_sun_illuminance = view_ray_intersects_ground ? 
        float3(0.0) : 
        get_sun_illuminance(
            settings,
            AT.transmittance,
            AT.globals->samplers.linear_clamp,
            view_direction,
            length(world_position),
            dot(settings->sun_direction, normalize(world_position))
        );
    return direct_sun_illuminance;
}

// ndc going in needs to be in range [-1, 1]
float3 get_view_direction(float2 ndc_xy)
{
    float3 world_direction; 
    if(AT.globals->settings.draw_from_observer == 1)
    {
        const float3 camera_position = AT.globals->observer_camera.position;
        const float4 unprojected_pos = mul(AT.globals->observer_camera.inv_view_proj, float4(ndc_xy, 1.0, 1.0));
        world_direction = normalize((unprojected_pos.xyz / unprojected_pos.w) - camera_position);
    }
    else 
    {
        const float3 camera_position = AT.globals->camera.position;
        const float4 unprojected_pos = mul(AT.globals->camera.inv_view_proj, float4(ndc_xy, 1.0, 1.0));
        world_direction = normalize((unprojected_pos.xyz / unprojected_pos.w) - camera_position);
    }
    return world_direction;
}

float3 get_vsm_debug_page_color(float2 uv, float depth, float3 world_position)
{
    float3 color = float3(1.0, 1.0, 1.0);

    const float4x4 inv_projection_view = AT.globals->camera.inv_view_proj;
    const bool level_forced = AT.globals->vsm_settings.force_clip_level != 0;
    const int force_clip_level = level_forced ? AT.globals->vsm_settings.forced_clip_level : -1;

    ClipInfo clip_info;
    uint2 render_target_size = AT.globals->settings.render_target_size;
    float real_depth = depth;
    float2 real_uv = uv;
    if(AT.globals->settings.draw_from_observer == 1u)
    {
        const float4 main_cam_proj_world = mul(AT.globals->camera.view_proj, float4(world_position, 1.0));
        const float2 ndc = main_cam_proj_world.xy / main_cam_proj_world.w;
        if(main_cam_proj_world.w < 0.0 || abs(ndc.x) > 1.0 || abs(ndc.y) > 1.0)
        {
            return float3(1.0);
        }
        real_uv = (ndc + float2(1.0)) / float2(2.0);
        real_depth = main_cam_proj_world.z / main_cam_proj_world.w;
    }
    clip_info = clip_info_from_uvs(ClipFromUVsInfo(
        real_uv,
        render_target_size,
        real_depth,
        inv_projection_view,
        force_clip_level,
        AT.vsm_clip_projections,
        AT.vsm_globals,
        AT.globals
    ));
    if(clip_info.clip_level >= VSM_CLIP_LEVELS) { return color; }

    const daxa_i32vec3 vsm_page_pix_coords = daxa_i32vec3(daxa_i32vec2(floor(clip_info.clip_depth_uv * VSM_PAGE_TABLE_RESOLUTION)), clip_info.clip_level);
    const uint is_dynamic_invalidated = unwrap_vsm_page_from_mask(vsm_page_pix_coords, AT.vsm_wrapped_pages);
    const int3 vsm_page_texel_coords = vsm_clip_info_to_wrapped_coords(clip_info, AT.vsm_clip_projections);
    const uint page_entry = Texture2DArray<uint>::get(AT.vsm_page_table).Load(int4(vsm_page_texel_coords, 0)).r;

    if(get_is_allocated(page_entry))
    {
        const int2 physical_page_coords = get_meta_coords_from_vsm_entry(page_entry);
        const int2 physical_texel_coords = virtual_uv_to_physical_texel(clip_info.clip_depth_uv, physical_page_coords);
        uint overdraw_amount = 0;
        if (AT.globals->settings.debug_draw_mode == DEBUG_DRAW_MODE_VSM_OVERDRAW)
        {
            overdraw_amount = RWTexture2D<uint>::get(AT.vsm_overdraw_debug)[physical_texel_coords].x;
        }
        const int2 in_page_texel_coords = int2(_mod(physical_texel_coords, float(VSM_PAGE_SIZE)));
        bool texel_near_border = any(greaterThan(in_page_texel_coords, int2(VSM_PAGE_SIZE - 1))) ||
                                 any(lessThan(in_page_texel_coords, int2(1)));
        if(texel_near_border)
        {
            color = float3(0.001, 0.001, 0.001);
        }
        else if(is_dynamic_invalidated != 0)
        {
            color.rgb = float3(1.0, 0.0, 1.0);
        }
        else
        {
            if(get_is_visited_marked(page_entry)) 
            {
                color.rgb = hsv2rgb(float3(pow(float(vsm_page_texel_coords.z) / float(VSM_CLIP_LEVELS - 1), 0.5), 1.0, 1.0));
            }
            else 
            {
                color.rgb = hsv2rgb(float3(pow(float(vsm_page_texel_coords.z) / float(VSM_CLIP_LEVELS - 1), 0.5), 0.8, 0.2));
            }
        }
        if (AT.globals->settings.debug_draw_mode == DEBUG_DRAW_MODE_VSM_OVERDRAW)
        {
            const float3 overdraw_color = 3.0 * TurboColormap(float(overdraw_amount) / 25.0);
            color.rgb = overdraw_color;
        }
    } else {
        color = float3(1.0, 0.0, 0.0);
        if(get_is_dirty(page_entry)) {color = float3(0.0, 0.0, 1.0);}
    }
    return color;
}

[[vk::binding(DAXA_STORAGE_IMAGE_BINDING, 0)]] RWTexture2D<daxa::u64> Texture2Duint64view[];

float vsm_shadow_test(ClipInfo clip_info, uint page_entry, float3 world_position, float3 page_camera_position, float sun_norm_dot, float2 screen_uv)
{
    const int2 physical_page_coords = get_meta_coords_from_vsm_entry(page_entry);
    const int2 physical_texel_coords = virtual_uv_to_physical_texel(clip_info.clip_depth_uv, physical_page_coords);

    float vsm_sample = 0.0f;
    if (push_opaque.attachments.attachments.globals.vsm_settings.use64bit != 0)
    {
        // TODO
        // const float vsm_sample = asfloat(uint(Texture2Duint64view[AT.vsm_memory_block64.index()].Load(int3(physical_texel_coords, 0)).r));
    }
    else
    {
        vsm_sample = Texture2D<float>::get(AT.vsm_memory_block).Load(int3(physical_texel_coords, 0)).r;
    }

    const float4x4 vsm_shadow_view = deref_i(AT.vsm_clip_projections, clip_info.clip_level).camera.view;

    float4x4 vsm_shifted_shadow_view = vsm_shadow_view;
    vsm_shifted_shadow_view[0][3] = page_camera_position[0]; 
    vsm_shifted_shadow_view[1][3] = page_camera_position[1]; 
    vsm_shifted_shadow_view[2][3] = page_camera_position[2]; 

    const float4x4 vsm_shadow_proj = deref_i(AT.vsm_clip_projections, clip_info.clip_level).camera.proj;

    const float3 view_projected_world_pos = (mul(vsm_shifted_shadow_view, daxa_f32vec4(world_position, 1.0))).xyz;

    const float view_space_offset = 0.04;// / abs(sun_norm_dot);//0.004 * pow(2.0, clip_info.clip_level);// / max(abs(sun_norm_dot), 0.05);
    const float3 offset_view_pos = float3(view_projected_world_pos.xy, view_projected_world_pos.z + view_space_offset);

    const float4 vsm_projected_world = mul(vsm_shadow_proj, float4(offset_view_pos, 1.0));
    const float vsm_projected_depth = vsm_projected_world.z / vsm_projected_world.w;

    const bool is_in_shadow = vsm_sample < vsm_projected_depth;
    return is_in_shadow ? 0.0 : 1.0;
}

float get_vsm_shadow(float2 uv, float depth, float3 world_position, float sun_norm_dot)
{
    const bool level_forced = AT.globals->vsm_settings.force_clip_level != 0;
    const int force_clip_level = level_forced ? AT.globals->vsm_settings.forced_clip_level : -1;

    const float4x4 inv_projection_view = AT.globals->camera.inv_view_proj;
    uint2 render_target_size = AT.globals->settings.render_target_size;
    ClipInfo clip_info;
    float real_depth = depth;
    float2 real_uv = uv;
    if(AT.globals->settings.draw_from_observer == 1u)
    {
        const float4 main_cam_proj_world = mul(AT.globals->camera.view_proj, float4(world_position, 1.0));
        const float2 ndc = main_cam_proj_world.xy / main_cam_proj_world.w;
        real_uv = (ndc + float2(1.0)) / float2(2.0);
        real_depth = main_cam_proj_world.z / main_cam_proj_world.w;
    }
    let base_clip_info = ClipFromUVsInfo(
        real_uv,
        render_target_size,
        real_depth,
        inv_projection_view,
        force_clip_level,
        AT.vsm_clip_projections,
        AT.vsm_globals,
        AT.globals
    );
    clip_info = clip_info_from_uvs(base_clip_info);
    if(clip_info.clip_level >= VSM_CLIP_LEVELS) { return 1.0; }

    const float filter_radius = 0.01;
    const int clip_levels[3] = {
        clip_info.clip_level,
        max(clip_info.clip_level - 1, 0),
        min(clip_info.clip_level + 1, VSM_CLIP_LEVELS - 1)
    };
    float sum = 0.0;

    rand_seed(asuint(uv.x + uv.y * 13136.1235f));
    float rand_angle = rand();
    [[unroll]]
    for(int sample = 0; sample < PCF_NUM_SAMPLES; sample++)
    {
        let filter_rot_offset = float2(
            poisson_disk[sample].x * cos(rand_angle) - poisson_disk[sample].y * sin(rand_angle),
            poisson_disk[sample].y * cos(rand_angle) + poisson_disk[sample].x * sin(rand_angle),
        );

        // for(int level = 0; level < 1; level++)
        // {
            let level = 0;
            let filter_view_space_offset = float4(filter_rot_offset * filter_radius * pow(1.0, clip_levels[level]), 0.0, 0.0);
            const daxa_f32vec3 center_world_space = world_space_from_uv( real_uv, real_depth, inv_projection_view);

            let clip_proj = AT.vsm_clip_projections[clip_levels[level]].camera.proj;
            let clip_view = AT.vsm_clip_projections[clip_levels[level]].camera.view;

            let view_space_world_pos = mul(clip_view, float4(world_position, 1.0));
            let view_space_offset_world_pos = view_space_world_pos + filter_view_space_offset;
            let proj_filter_offset_world = mul(clip_proj, view_space_offset_world_pos);

            let clip_uv = ((proj_filter_offset_world.xy / proj_filter_offset_world.w) + 1.0) / 2.0;
            let offset_info = ClipInfo(clip_levels[level], clip_uv);

            if(all(greaterThanEqual(offset_info.clip_depth_uv, 0.0)) && all(lessThan(offset_info.clip_depth_uv, 1.0)))
            {
                let vsm_page_texel_coords = vsm_clip_info_to_wrapped_coords(offset_info, AT.vsm_clip_projections);
                let page_entry = Texture2DArray<uint>::get(AT.vsm_page_table).Load(int4(vsm_page_texel_coords, 0)).r;
                const float3 page_camera_pos = Texture2DArray<float3>::get(AT.vsm_page_view_pos_row).Load(int4(vsm_page_texel_coords, 0));

                if(get_is_allocated(page_entry))
                {
                    sum += vsm_shadow_test(offset_info, page_entry, world_position, page_camera_pos, sun_norm_dot, uv);
                    // break;
                }
            }
        // }
    }
    return sum / PCF_NUM_SAMPLES;
}

[[vk::binding(DAXA_STORAGE_IMAGE_BINDING, 0)]] RWTexture2D<daxa::u64> tex_u64_table[];

[numthreads(SHADE_OPAQUE_WG_X, SHADE_OPAQUE_WG_Y, 1)]
[shader("compute")]
void entry_main_cs(
    uint3 svdtid : SV_DispatchThreadID
)
{
    let push = push_opaque;
    const int2 index = svdtid.xy;
    const float2 screen_uv = (float2(svdtid.xy) + 0.5f) * AT.globals->settings.render_target_size_inv;

    const float3 atmo_camera_position = AT.globals->settings.draw_from_observer == 1 ? 
        AT.globals->observer_camera.position * M_TO_KM_SCALE :
        AT.globals->camera.position * M_TO_KM_SCALE;
    const float3 bottom_atmo_offset = float3(0,0, AT.globals->sky_settings.atmosphere_bottom + BASE_HEIGHT_OFFSET);
    const float3 bottom_atmo_offset_camera_position = atmo_camera_position + bottom_atmo_offset;

    uint triangle_id;
    if(all(lessThan(index, AT.globals->settings.render_target_size)))
    {
        triangle_id = Texture2D<uint>::get(AT.vis_image).Load(int3(index, 0), int2(0)).x;
    } else {
        triangle_id = INVALID_TRIANGLE_ID;
    }

    float4 output_value = float4(0);
    float4 debug_value = float4(0);

    bool triangle_id_valid = triangle_id != INVALID_TRIANGLE_ID;

    float4x4 view_proj;
    float3 camera_position;
    if(AT.globals->settings.draw_from_observer == 1)
    {
        view_proj = AT.globals->observer_camera.view_proj;
        camera_position = AT.globals->observer_camera.position;
    }
    else 
    {
        view_proj = AT.globals->camera.view_proj;
        camera_position = AT.globals->camera.position;
    }

    let pixel_ndc = float4(screen_uv * 2.0f - 1.0f, 0.0f, 1.0f);
    let primary_ray = normalize(mul(AT.globals.camera.inv_view_proj, pixel_ndc).xyz);
    float world_space_depth = 0.0f;

    if(triangle_id_valid)
    {
        daxa_BufferPtr(MeshletInstancesBufferHead) instantiated_meshlets = AT.instantiated_meshlets;
        daxa_BufferPtr(GPUMesh) meshes = AT.meshes;
        daxa_BufferPtr(daxa_f32mat4x3) combined_transforms = AT.combined_transforms;
        VisbufferTriangleData tri_data = visgeo_triangle_data(
            triangle_id,
            float2(index),
            push.size,
            push.inv_size,
            view_proj,
            instantiated_meshlets,
            meshes,
            combined_transforms
        );        

        world_space_depth = length(tri_data.world_position - camera_position);

        float3 normal = tri_data.world_normal;
        GPUMaterial material;
        material.diffuse_texture_id.value = 0;
        material.normal_texture_id.value = 0;
        material.roughnes_metalness_id.value = 0;
        material.alpha_discard_enabled = false;
        material.normal_compressed_bc5_rg = false;
        material.base_color = float3(1.0);
        if(tri_data.meshlet_instance.material_index != INVALID_MANIFEST_INDEX)
        {
            material = AT.material_manifest[tri_data.meshlet_instance.material_index];
        }

        float3 albedo = float3(material.base_color);
        if(material.diffuse_texture_id.value != 0)
        {
            albedo = Texture2D<float4>::get(material.diffuse_texture_id).SampleGrad(
                // SamplerState::get(AT.globals->samplers.nearest_repeat_ani),
                SamplerState::get(AT.globals->samplers.linear_repeat_ani),
                tri_data.uv, tri_data.uv_ddx, tri_data.uv_ddy
            ).rgb;
        }

        if(material.normal_texture_id.value != 0)
        {
            float3 normal_map_value = float3(0);
            if(material.normal_compressed_bc5_rg)
            {
                const float2 raw = Texture2D<float4>::get(material.normal_texture_id).SampleGrad(
                    SamplerState::get(AT.globals->samplers.normals),
                    tri_data.uv, tri_data.uv_ddx, tri_data.uv_ddy
                ).rg;
                const float2 rescaled_normal_rg = raw * 2.0f - 1.0f;
                const float normal_b = sqrt(clamp(1.0f - dot(rescaled_normal_rg, rescaled_normal_rg), 0.0, 1.0));
                normal_map_value = float3(rescaled_normal_rg, normal_b);
            }
            else
            {
                const float3 raw = Texture2D<float4>::get(material.normal_texture_id).SampleGrad(
                    SamplerState::get(AT.globals->samplers.normals),
                    tri_data.uv, tri_data.uv_ddx, tri_data.uv_ddy
                ).rgb;
                normal_map_value = raw * 2.0f - 1.0f;
            }
            const float3x3 tbn = transpose(float3x3(-tri_data.world_tangent, -cross(tri_data.world_tangent, tri_data.world_normal), tri_data.world_normal));
            normal = mul(tbn, normal_map_value);
        }

        const float3 sun_direction = AT.globals->sky_settings.sun_direction;
        const float sun_norm_dot = clamp(dot(normal, sun_direction), 0.0, 1.0);
        const float vsm_shadow = AT.globals->vsm_settings.enable != 0 ? get_vsm_shadow(screen_uv, tri_data.depth, tri_data.world_position, sun_norm_dot) : 1.0f;
        const float final_shadow = sun_norm_dot * vsm_shadow.x;

        const float3 atmo_camera_position = AT.globals->camera.position * M_TO_KM_SCALE;

        const float3 direct_lighting = final_shadow * get_sun_direct_lighting(AT.globals->sky_settings_ptr, sun_direction, bottom_atmo_offset_camera_position);
        const float4 compressed_indirect_lighting = TextureCube<float4>::get(AT.sky_ibl).SampleLevel(SamplerState::get(AT.globals->samplers.linear_clamp), normal, 0);
        float ambient_occlusion = 1.0f;
        const bool ao_enabled = (AT.globals.settings.ao_mode != AO_MODE_NONE) && !AT.ao_image.id.is_empty();
        if (ao_enabled)
        {
            ambient_occlusion = AT.ao_image.get().Load(index);
        }
        const float3 indirect_lighting = compressed_indirect_lighting.rgb * compressed_indirect_lighting.a;
        const float3 lighting = direct_lighting + (indirect_lighting * ambient_occlusion);

        let shaded_color = albedo.rgb * lighting;

        float3 dummy_color = float3(1,0,1);
        uint id_to_visualize = ~0u;
        switch(AT.globals->settings.debug_draw_mode)
        {
            case DEBUG_DRAW_MODE_OVERDRAW:
            {
                if (AT.overdraw_image.value != 0)
                {
                    let value = Texture2D<uint>::get(AT.overdraw_image)[index].x;
                    let scaled_value = float(value) * AT.globals->settings.debug_overdraw_scale;
                    let color = TurboColormap(scaled_value);
                    output_value.rgb = color;
                }
                break;
            }
            case DEBUG_DRAW_MODE_TRIANGLE_INSTANCE_ID: id_to_visualize = tri_data.meshlet_instance.entity_index * 100 + tri_data.meshlet_instance.meshlet_index * 10 + tri_data.meshlet_triangle_index; break;
            case DEBUG_DRAW_MODE_MESHLET_INSTANCE_ID: id_to_visualize = tri_data.meshlet_instance.entity_index * 100 + tri_data.meshlet_instance.meshlet_index; break;
            case DEBUG_DRAW_MODE_ENTITY_ID: id_to_visualize = tri_data.meshlet_instance.entity_index; break;
            case DEBUG_DRAW_MODE_VSM_OVERDRAW: 
            {
                let vsm_debug_color = get_vsm_debug_page_color(screen_uv, tri_data.depth, tri_data.world_position);
                output_value.rgb = vsm_debug_color;
                break;
            }
            case DEBUG_DRAW_MODE_VSM_CLIP_LEVEL: 
            {
                if (AT.globals->vsm_settings.enable != 0)
                {
                    let vsm_debug_color = get_vsm_debug_page_color(screen_uv, tri_data.depth, tri_data.world_position);
                    let debug_albedo = albedo.rgb * lighting * vsm_debug_color;
                    output_value.rgb = debug_albedo;
                }
                break;
            }
            case DEBUG_DRAW_MODE_DEPTH:
            {
                float depth = tri_data.depth;
                let color = unband_z_color(index.x, index.y, linearise_depth(AT.globals.camera.near_plane, depth));
                output_value.rgb = color;
                break;
            }
            case DEBUG_DRAW_MODE_ALBEDO:
            {
                output_value.rgb = albedo;
                break;
            }
            case DEBUG_DRAW_MODE_NORMAL:
            {
                let color = (normal * 0.5 + 0.5f);
                output_value.rgb = color;
                break;
            }
            case DEBUG_DRAW_MODE_LIGHT:
            {
                output_value.rgb = lighting;
                break;
            }
            case DEBUG_DRAW_MODE_AO:
            {
                output_value.rgb = ambient_occlusion;
                break;
            }
            case DEBUG_DRAW_MODE_NONE:
            default:
            output_value.rgb = shaded_color;
            break;
        }
        if (id_to_visualize != ~0u)
        {
            output_value.rgb = hsv2rgb(float3(float(id_to_visualize) * 0.1323f, 1, 1));
        }
    }
    else 
    {
        world_space_depth = VOLUMETRIC_SKY_DEPTH;

        const float2 ndc_xy = screen_uv * 2.0 - 1.0;
        const float3 view_direction = get_view_direction(ndc_xy);
        const float3 atmosphere_direct_illuminnace = get_atmosphere_illuminance_along_ray(
            AT.globals->sky_settings_ptr,
            AT.transmittance,
            AT.sky,
            AT.globals->samplers.linear_clamp,
            view_direction,
            bottom_atmo_offset_camera_position
        );
        const float3 sun_direct_illuminance = get_sun_direct_lighting(AT.globals->sky_settings_ptr, view_direction, bottom_atmo_offset_camera_position);
        const float3 total_direct_illuminance = sun_direct_illuminance + atmosphere_direct_illuminnace;
        output_value.rgb = total_direct_illuminance;
        debug_value.xyz = atmosphere_direct_illuminnace;
    }

    
    //const uint thread_seed = (index.x * AT.globals->settings.render_target_size.y + index.y) * AT.globals.frame_index;
    //rand_seed(thread_seed);
    //let sun_direction = AT.globals->sky_settings.sun_direction;
    //VolumetricResult volumetric = volumetric_extinction_inscatter(camera_position, primary_ray, world_space_depth, daxa::acceleration_structures[AT.tlas.index()], sun_direction);
    //let endpoint = camera_position + primary_ray * world_space_depth;
    //output_value.rgb = output_value.rgb * volumetric.transmittance + volumetric.inscattering;

    //RWTexture2D<float4>::get(AT.debug_image)[index] = float4(frac(endpoint), 1.0f);

    const float exposure = compute_exposure(deref(AT.luminance_average));
    float3 exposed_color = output_value.rgb * exposure;
    debug_write_lens(
        AT.globals->debug,
        AT.debug_lens_image,
        index,
        float4(exposed_color, 1.0f),
    );
    
    AT.color_image.get()[index] = exposed_color;
}