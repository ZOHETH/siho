#include "application.h"
#include "common/vk_common.h"
#include "glm/gtc/type_ptr.hpp"

#include "rendering/pipeline_state.h"
#include "rendering/render_context.h"
#include "rendering/render_pipeline.h"
#include "rendering/subpasses/geometry_subpass.h"
#include "rendering/subpasses/lighting_subpass.h"
#include "scene_graph/node.h"

namespace siho
{

	LightingSubpass::LightingSubpass(vkb::RenderContext& render_context, vkb::ShaderSource&& vertex_shader,
		vkb::ShaderSource&& fragment_shader, vkb::sg::Camera& camera, vkb::sg::Scene& scene,
		vkb::sg::Camera& shadowmap_camera, std::vector<std::unique_ptr<vkb::RenderTarget>>& shadow_render_targets)

		:vkb::LightingSubpass(render_context, std::move(vertex_shader), std::move(fragment_shader), camera, scene),
		shadowmap_camera(shadowmap_camera), shadow_render_targets(shadow_render_targets)
	{
	}

	void LightingSubpass::prepare()
	{
		vkb::LightingSubpass::prepare();
		VkSamplerCreateInfo shadowmap_sampler_create_info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
		shadowmap_sampler_create_info.minFilter = VK_FILTER_LINEAR;
		shadowmap_sampler_create_info.magFilter = VK_FILTER_LINEAR;
		shadowmap_sampler_create_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		shadowmap_sampler_create_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		shadowmap_sampler_create_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		shadowmap_sampler_create_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
		shadowmap_sampler_create_info.compareEnable = VK_TRUE;
		shadowmap_sampler_create_info.compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
		shadowmap_sampler = std::make_unique<vkb::core::Sampler>(get_render_context().get_device(), shadowmap_sampler_create_info);
	}

	void LightingSubpass::draw(vkb::CommandBuffer& command_buffer)
	{
		ShadowUniform shadow_uniform;
		shadow_uniform.shadowmap_projection_matrix = vkb::vulkan_style_projection(shadowmap_camera.get_projection()) * shadowmap_camera.get_view();

		auto& shadow_render_target = *shadow_render_targets[get_render_context().get_active_frame_index()];
		assert(!shadow_render_target.get_views().empty());
		command_buffer.bind_image(shadow_render_target.get_views().at(0), *shadowmap_sampler, 0, 5, 0);

		auto& render_frame = get_render_context().get_active_frame();
		vkb::BufferAllocation shadow_buffer = render_frame.allocate_buffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, sizeof(glm::mat4));
		shadow_buffer.update(shadow_uniform);
		command_buffer.bind_buffer(shadow_buffer.get_buffer(), shadow_buffer.get_offset(), shadow_buffer.get_size(), 0, 6, 0);
		vkb::LightingSubpass::draw(command_buffer);
	}


	bool Application::prepare(const vkb::ApplicationOptions& options)
	{
		if (!VulkanSample::prepare(options))
		{
			return false;
		}

		shadow_render_targets.resize(get_render_context().get_render_frames().size());
		for (uint32_t i = 0; i < shadow_render_targets.size(); i++)
		{
			shadow_render_targets[i] = create_shadow_render_target(SHADOWMAP_RESOLUTION);
		}

		std::set<VkImageUsageFlagBits> usage = { VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT ,
			VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT };
		get_render_context().update_swapchain(usage);

		load_scene("scenes/sponza/Sponza01.gltf");

		scene->clear_components<vkb::sg::Light>();

		auto& directional_light = vkb::add_directional_light(*scene, glm::quat({ glm::radians(-30.0f), glm::radians(-85.0f), glm::radians(0.0f) }));
		auto& directional_light_transform = directional_light.get_node()->get_transform();
		directional_light_transform.set_translation(glm::vec3(0, 0, -400));

		// Attach a camera component to the light node
		auto shadowmap_camera_ptr = std::make_unique<vkb::sg::OrthographicCamera>("shadowmap_camera", -850.0f, 850.0f, -800.0f, 800.0f, -1000.0f, 1500.0f);
		shadowmap_camera_ptr->set_node(*directional_light.get_node());
		shadowmap_camera = shadowmap_camera_ptr.get();
		directional_light.get_node()->set_component(*shadowmap_camera_ptr);
		scene->add_component(std::move(shadowmap_camera_ptr));

		auto light_pos = glm::vec3(0.0f, 128.0f, -225.0f);
		auto light_color = glm::vec3(1.0, 1.0, 1.0);

		for (int i = -1; i < 4; ++i)
		{
			for (int j = 0; j < 2; ++j)
			{
				glm::vec3 pos = light_pos;
				pos.x += i * 400.0f;
				pos.z += j * (225 + 140);
				pos.y = 8;

				for (int k = 0; k < 3; ++k)
				{
					pos.y = pos.y + (k * 100);

					light_color.x = static_cast<float>(rand()) / (RAND_MAX);
					light_color.y = static_cast<float>(rand()) / (RAND_MAX);
					light_color.z = static_cast<float>(rand()) / (RAND_MAX);

					vkb::sg::LightProperties props;
					props.color = light_color;
					props.intensity = 0.2f;

					vkb::add_point_light(*scene, pos, props);
				}
			}
		}

		auto& camera_node = vkb::add_free_camera(*scene, "main_camera", get_render_context().get_surface_extent());
		camera = dynamic_cast<vkb::sg::PerspectiveCamera*>(&camera_node.get_component<vkb::sg::Camera>());

		shadow_render_pipeline = create_shadow_renderpass();
		render_pipeline = create_render_pipeline();

		stats->request_stats({ vkb::StatIndex::frame_times });

		gui = std::make_unique<vkb::Gui>(*this, *window, stats.get());

		return true;

	}

	void Application::update(float delta_time)
	{
		update_scene(delta_time);
		update_stats(delta_time);
		update_gui(delta_time);

		auto& main_command_buffer = render_context->begin();

		auto command_buffers = record_command_buffers(main_command_buffer);

		render_context->submit(command_buffers);
	}

	void Application::draw_gui()
	{
		const bool landscape = camera->get_aspect_ratio() > 1.0f;
		uint32_t lines = 6;


		gui->show_options_window(
			[this]() {
				ImGui::AlignTextToFramePadding();
				ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.4f);

				auto directional_light = scene->get_components<vkb::sg::Light>()[0];
				auto& transform = directional_light->get_node()->get_transform();
				glm::quat rotation = transform.get_rotation();
				glm::vec3 euler_angles = glm::eulerAngles(rotation);
				float pitch_limit = 89.0f;
				euler_angles.y = glm::clamp(euler_angles.y, -pitch_limit, pitch_limit);
				euler_angles = glm::degrees(euler_angles);

				if (ImGui::DragFloat3("Rotation", glm::value_ptr(euler_angles), 0.1f, -180.0f, 180.0f))
				{
					euler_angles.y = glm::clamp(euler_angles.y, -pitch_limit, pitch_limit);
					euler_angles = glm::radians(euler_angles);
					transform.set_rotation(glm::quat(euler_angles));
				}
				glm::vec3 position = transform.get_translation();
				if (ImGui::DragFloat3("Position", &position.x))
				{
					transform.set_translation(position);
				}

				float orthoParams[6] = { shadowmap_camera->get_left(), shadowmap_camera->get_right(),
										shadowmap_camera->get_bottom(), shadowmap_camera->get_top(),
										shadowmap_camera->get_near_plane(), shadowmap_camera->get_far_plane() };

				if (ImGui::DragFloat4("Ortho Params (Left/Right/Bottom/Top)", orthoParams))
				{
					shadowmap_camera->set_left(orthoParams[0]);
					shadowmap_camera->set_right(orthoParams[1]);
					shadowmap_camera->set_bottom(orthoParams[2]);
					shadowmap_camera->set_top(orthoParams[3]);
				}

				if (ImGui::DragFloat2("Ortho Params (Near/Far)", &orthoParams[4]))
				{
					shadowmap_camera->set_near_plane(orthoParams[4]);
					shadowmap_camera->set_far_plane(orthoParams[5]);
				}
				ImGui::PopItemWidth();
			},
			lines);
	}

	void Application::prepare_render_context()
	{
		get_render_context().prepare(2, [this](vkb::core::Image&& swapchain_image)
			{
				return create_render_target(std::move(swapchain_image));
			});
	}

	std::unique_ptr<vkb::RenderTarget> Application::create_shadow_render_target(uint32_t size) const
	{
		VkExtent3D extent{ size, size, 1 };

		vkb::core::Image depth_image{
			*device,
			extent,
			vkb::get_suitable_depth_format(device->get_gpu().get_handle()),
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			VMA_MEMORY_USAGE_GPU_ONLY
		};
		std::vector<vkb::core::Image> images;
		images.push_back(std::move(depth_image));
		return std::make_unique<vkb::RenderTarget>(std::move(images));
	}

	std::unique_ptr<vkb::RenderPipeline> Application::create_shadow_renderpass()
	{
		auto shadowmap_vs = vkb::ShaderSource{ "shadows/shadowmap.vert" };
		auto shadowmap_fs = vkb::ShaderSource{ "shadows/shadowmap.frag" };
		auto scene_subpass = std::make_unique<ShadowSubpass>(get_render_context(), std::move(shadowmap_vs), std::move(shadowmap_fs), *scene, *shadowmap_camera);

		shadow_subpass = scene_subpass.get();

		auto shadowmap_render_pipeline = std::make_unique<vkb::RenderPipeline>();
		shadowmap_render_pipeline->add_subpass(std::move(scene_subpass));

		return shadowmap_render_pipeline;
	}

	std::unique_ptr<vkb::RenderTarget> Application::create_render_target(vkb::core::Image&& swapchain_image) const
	{
		auto& device = swapchain_image.get_device();
		auto& extent = swapchain_image.get_extent();
		// G-Buffer should fit 128-bit budget for buffer color storage
		// in order to enable subpasses merging by the driver
		// Light (swapchain_image) RGBA8_UNORM   (32-bit)
		// Albedo                  RGBA8_UNORM   (32-bit)
		// Normal                  RGB10A2_UNORM (32-bit)
		vkb::core::Image depth_image{ device,extent,
			vkb::get_suitable_depth_format(swapchain_image.get_device().get_gpu().get_handle()),
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | rt_usage_flags,
			VMA_MEMORY_USAGE_GPU_ONLY
		};

		vkb::core::Image albedo_image{ device,extent,
			albedo_format,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | rt_usage_flags,
			VMA_MEMORY_USAGE_GPU_ONLY
		};

		vkb::core::Image normal_image{ device,extent,
			normal_format,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | rt_usage_flags,
			VMA_MEMORY_USAGE_GPU_ONLY
		};

		std::vector<vkb::core::Image> images;

		// Attachment 0
		images.push_back(std::move(swapchain_image));

		// Attachment 1
		images.push_back(std::move(depth_image));

		// Attachment 2
		images.push_back(std::move(albedo_image));

		// Attachment 3
		images.push_back(std::move(normal_image));

		return std::make_unique<vkb::RenderTarget>(std::move(images));
	}

	std::unique_ptr<vkb::RenderPipeline> Application::create_render_pipeline()
	{
		// Geometry subpass
		auto geometry_vs = vkb::ShaderSource{ "deferred/geometry.vert" };
		auto geometry_fs = vkb::ShaderSource{ "deferred/geometry.frag" };
		auto scene_subpass = std::make_unique<vkb::GeometrySubpass>(get_render_context(), std::move(geometry_vs), std::move(geometry_fs), *scene, *camera);

		// Outputs are depth, albedo, normal
		scene_subpass->set_output_attachments({ 1, 2, 3 });

		auto lighting_vs = vkb::ShaderSource{ "deferred/lighting.vert" };
		auto lighting_fs = vkb::ShaderSource{ "deferred/lighting.frag" };
		auto lighting_subpass = std::make_unique<LightingSubpass>(get_render_context(), std::move(lighting_vs), std::move(lighting_fs), *camera, *scene,*shadowmap_camera, shadow_render_targets);

		lighting_subpass->set_input_attachments({ 1, 2, 3 });

		std::vector<std::unique_ptr<vkb::Subpass>> subpasses{};
		subpasses.push_back(std::move(scene_subpass));
		subpasses.push_back(std::move(lighting_subpass));

		auto render_pipeline = std::make_unique<vkb::RenderPipeline>(std::move(subpasses));

		render_pipeline->set_load_store(vkb::gbuffer::get_clear_all_store_swapchain());
		render_pipeline->set_clear_value(vkb::gbuffer::get_clear_value());

		return render_pipeline;
	}

	void Application::draw_shadow_pass(vkb::CommandBuffer& command_buffer)
	{
		auto& shadow_render_target = *shadow_render_targets[get_render_context().get_active_frame_index()];
		auto& shadowmap_extent = shadow_render_target.get_extent();

		set_viewport_and_scissor(command_buffer, shadowmap_extent);

		if (command_buffer.level == VK_COMMAND_BUFFER_LEVEL_SECONDARY)
		{
			shadow_render_pipeline->draw(command_buffer, shadow_render_target);
		}
		else
		{
			record_shadow_pass_image_memory_barriers(command_buffer);
			shadow_render_pipeline->draw(command_buffer, shadow_render_target);
			command_buffer.end_render_pass();
		}
	}

	void Application::draw_main_pass(vkb::CommandBuffer& command_buffer)
	{
		auto& render_target = render_context->get_active_frame().get_render_target();
		auto& extent = render_target.get_extent();

		set_viewport_and_scissor(command_buffer, extent);

		if (command_buffer.level == VK_COMMAND_BUFFER_LEVEL_SECONDARY)
		{
			render_pipeline->draw(command_buffer, render_target);
		}
		else
		{
			record_main_pass_image_memory_barriers(command_buffer);
			render_pipeline->draw(command_buffer, render_target);
			if (gui)
			{
				gui->draw(command_buffer);
			}
			command_buffer.end_render_pass();
			record_present_image_memory_barriers(command_buffer);
		}


	}

	std::vector<vkb::CommandBuffer*> Application::record_command_buffers(vkb::CommandBuffer& main_command_buffer)
	{
		auto reset_mode = vkb::CommandBuffer::ResetMode::ResetPool;
		const auto& queue = device->get_queue_by_flags(VK_QUEUE_GRAPHICS_BIT, 0);

		std::vector<vkb::CommandBuffer*> command_buffers;
		shadow_subpass->set_thread_index(1);

		// auto& scene_command_buffer = render_context->get_active_frame().request_command_buffer(queue, reset_mode, VK_COMMAND_BUFFER_LEVEL_SECONDARY, 0);
		// auto& shadow_command_buffer = render_context->get_active_frame().request_command_buffer(queue, reset_mode, VK_COMMAND_BUFFER_LEVEL_SECONDARY, 1);

		main_command_buffer.begin(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
		draw_shadow_pass(main_command_buffer);
		draw_main_pass(main_command_buffer);
		main_command_buffer.end();
		command_buffers.push_back(&main_command_buffer);

		return command_buffers;
	}

	void Application::record_main_pass_image_memory_barriers(vkb::CommandBuffer& command_buffer)
	{
		auto& views = render_context->get_active_frame().get_render_target().get_views();
		{
			vkb::ImageMemoryBarrier memory_barrier{};
			memory_barrier.old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
			memory_barrier.new_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			memory_barrier.src_access_mask = 0;
			memory_barrier.dst_access_mask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			memory_barrier.src_stage_mask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			memory_barrier.dst_stage_mask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

			assert(swapchain_attachment_index < views.size());
			command_buffer.image_memory_barrier(views[swapchain_attachment_index], memory_barrier);
			// Skip 1 as it is handled later as a depth-stencil attachment
			for (size_t i = 2; i < views.size(); ++i)
			{
				command_buffer.image_memory_barrier(views[i], memory_barrier);
			}
		}

		{
			vkb::ImageMemoryBarrier memory_barrier{};
			memory_barrier.old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
			memory_barrier.new_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			memory_barrier.src_access_mask = 0;
			memory_barrier.dst_access_mask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			memory_barrier.src_stage_mask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			memory_barrier.dst_stage_mask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;

			assert(depth_attachment_index < views.size());
			command_buffer.image_memory_barrier(views[depth_attachment_index], memory_barrier);
		}

		{
			assert(shadowmap_attachment_index < shadow_render_targets[render_context->get_active_frame_index()]->get_views().size());
			auto& shadowmap = shadow_render_targets[render_context->get_active_frame_index()]->get_views()[shadowmap_attachment_index];

			vkb::ImageMemoryBarrier memory_barrier{};
			memory_barrier.old_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			memory_barrier.new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			memory_barrier.src_access_mask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			memory_barrier.dst_access_mask = VK_ACCESS_SHADER_READ_BIT;
			memory_barrier.src_stage_mask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			memory_barrier.dst_stage_mask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

			command_buffer.image_memory_barrier(shadowmap, memory_barrier);
		}
	}

	void Application::record_shadow_pass_image_memory_barriers(vkb::CommandBuffer& command_buffer)
	{
		assert(shadowmap_attachment_index < shadow_render_targets[render_context->get_active_frame_index()]->get_views().size());
		auto& shadowmap = shadow_render_targets[render_context->get_active_frame_index()]->get_views()[shadowmap_attachment_index];

		vkb::ImageMemoryBarrier memory_barrier{};
		memory_barrier.old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		memory_barrier.new_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		memory_barrier.src_access_mask = 0;
		memory_barrier.dst_access_mask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		memory_barrier.src_stage_mask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		memory_barrier.dst_stage_mask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;

		command_buffer.image_memory_barrier(shadowmap, memory_barrier);
	}

	void Application::record_present_image_memory_barriers(vkb::CommandBuffer& command_buffer)
	{
		auto& views = render_context->get_active_frame().get_render_target().get_views();
		{
			vkb::ImageMemoryBarrier memory_barrier{};
			memory_barrier.old_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			memory_barrier.new_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			memory_barrier.src_access_mask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			memory_barrier.src_stage_mask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			memory_barrier.dst_stage_mask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

			assert(swapchain_attachment_index < views.size());
			command_buffer.image_memory_barrier(views[swapchain_attachment_index], memory_barrier);
		}
	}
}

std::unique_ptr<vkb::Application> create_application()
{
	return std::make_unique<siho::Application>();
}