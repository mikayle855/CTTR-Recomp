// [bt3] gs_dump_replayer with a PPM dump of every N-th scanout (offline picture checks against console dumps).
#include "gs_interface.hpp"
#include "gs_dump_parser.hpp"
#include "device.hpp"
#include "context.hpp"
#include "cli_parser.hpp"
#include "logging.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace ParallelGS;
using namespace Vulkan;
using namespace Util;

static void write_ppm(Device &device, const ScanoutResult &res, const char *path)
{
	if (!res.image) return;
	const uint32_t w = res.image->get_width(), h = res.image->get_height();
	BufferCreateInfo bi = {};
	bi.size = VkDeviceSize(w) * h * 4u;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bi.domain = BufferDomain::CachedHost;
	auto buf = device.create_buffer(bi);
	auto cmd = device.request_command_buffer();
	cmd->image_barrier(*res.image, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	cmd->copy_image_to_buffer(*buf, *res.image, 0, {}, { w, h, 1 }, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 });
	cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
	Fence fence;
	device.submit(cmd, &fence);
	fence->wait();
	const uint8_t *src = static_cast<const uint8_t *>(device.map_host_buffer(*buf, MEMORY_ACCESS_READ_BIT));
	const bool bgra = res.image->get_format() == VK_FORMAT_B8G8R8A8_UNORM || res.image->get_format() == VK_FORMAT_B8G8R8A8_SRGB;
	FILE *f = std::fopen(path, "wb");
	if (f)
	{
		std::fprintf(f, "P6\n%u %u\n255\n", w, h);
		std::vector<uint8_t> row(size_t(w) * 3u);
		for (uint32_t y = 0; y < h; y++)
		{
			const uint8_t *p = src + size_t(y) * w * 4u;
			for (uint32_t x = 0; x < w; x++) { row[x * 3 + 0] = p[x * 4 + (bgra ? 2 : 0)]; row[x * 3 + 1] = p[x * 4 + 1]; row[x * 3 + 2] = p[x * 4 + (bgra ? 0 : 2)]; }
			std::fwrite(row.data(), 1, row.size(), f);
		}
		std::fclose(f);
	}
	device.unmap_host_buffer(*buf, MEMORY_ACCESS_READ_BIT);
	// leave the image in the layout the renderer expects
	auto cmd2 = device.request_command_buffer();
	cmd2->image_barrier(*res.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
	                    VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	device.submit(cmd2);
}

int main(int argc, char **argv)
{
	std::string dump_path, prefix = "frame";
	unsigned every = 30, max_vsyncs = 0;
	bool high_res_scanout = false;
	GSOptions opts = {};
	CLICallbacks cbs;
	cbs.add("--ssaa", [&](CLIParser &parser) { opts.super_sampling = SuperSampling(parser.next_uint()); });
	cbs.add("--every", [&](CLIParser &parser) { every = parser.next_uint(); });
	cbs.add("--max", [&](CLIParser &parser) { max_vsyncs = parser.next_uint(); });
	cbs.add("--prefix", [&](CLIParser &parser) { prefix = parser.next_string(); });
	cbs.add("--high-res-scanout", [&](CLIParser &) { high_res_scanout = true; });
	cbs.default_handler = [&](const char *arg) { dump_path = arg; };
	CLIParser cli_parser(std::move(cbs), argc - 1, argv + 1);
	if (!cli_parser.parse() || dump_path.empty()) { LOGE("usage: replayer-ppm <dump.gs> [--ssaa N] [--every N] [--max N] [--prefix p]\n"); return EXIT_FAILURE; }
	if (!Context::init_loader(nullptr)) return EXIT_FAILURE;
	Context ctx;
	ctx.set_num_thread_indices(1);
	if (!ctx.init_instance_and_device(nullptr, 0, nullptr, 0,
	                                  CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT | CONTEXT_CREATION_ENABLE_DESCRIPTOR_HEAP_BIT | CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT))
		return EXIT_FAILURE;
	Device device;
	device.set_context(ctx);
	device.init_frame_contexts(4);
	GSInterface iface;
	if (!iface.init(&device, opts)) return EXIT_FAILURE;
	GSDumpParser parser;
	if (!parser.open(dump_path.c_str(), 4 * 1024 * 1024, &iface)) return EXIT_FAILURE;
	unsigned vsyncs = 0;
	while (parser.iterate_until_vsync(high_res_scanout, false))
	{
		ScanoutResult res = parser.consume_vsync_result();
		if (res.image && (vsyncs % every) == 0)
		{
			char path[512]; std::snprintf(path, sizeof(path), "%s_%05u.ppm", prefix.c_str(), vsyncs);
			write_ppm(device, res, path);
			LOGI("wrote %s (%ux%u fmt %d)\n", path, res.image->get_width(), res.image->get_height(), int(res.image->get_format()));
		}
		vsyncs++;
		if (max_vsyncs && vsyncs >= max_vsyncs) break;
	}
	LOGI("Done, %u vsyncs\n", vsyncs);
	return 0;
}
