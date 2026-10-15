/* ROM-free capability/output smoke for the recording dependency bundle.
 * This uses the real static FFmpeg libraries, not emulator/Qt objects.
 * It intentionally does not require decoders or demuxers: the official
 * recording dependency recipe disables both. */
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
	std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::exit(1); \
} } while (0)

static void requireFFmpeg(int result, const char* operation) {
	++checks;
	if (result >= 0) return;
	char error[AV_ERROR_MAX_STRING_SIZE];
	av_strerror(result, error, sizeof(error));
	std::fprintf(stderr, "FAIL %s: %s (%d)\n", operation, error, result);
	std::exit(1);
}

static void capabilities(void) {
	static const char* const encoders[] = {
		"libx264", "libx264rgb", "aac", "ffv1", "png", "gif",
		"libopus", "libvpx", "libvpx-vp9", "libmp3lame", "libwebp",
		"libwebp_anim", "libx265", "flac"
	};
	static const char* const filters[] = {
		"palettegen", "paletteuse", "scale", "split", "buffer", "buffersink"
	};
	static const char* const muxers[] = {"mp4", "webm", "gif", "matroska", "avi", "wav"};
	std::printf("FFmpeg %s: recording capability checks\n", av_version_info());
	for (const char* name : encoders) {
		if (!avcodec_find_encoder_by_name(name)) std::fprintf(stderr, "Missing encoder: %s\n", name);
		CHECK(avcodec_find_encoder_by_name(name));
		std::printf("ENCODER %s available\n", name);
	}
	for (const char* name : filters) {
		if (!avfilter_get_by_name(name)) std::fprintf(stderr, "Missing filter: %s\n", name);
		CHECK(avfilter_get_by_name(name));
		std::printf("FILTER %s available\n", name);
	}
	for (const char* name : muxers) {
		if (!av_guess_format(name, nullptr, nullptr)) std::fprintf(stderr, "Missing muxer: %s\n", name);
		CHECK(av_guess_format(name, nullptr, nullptr));
		std::printf("MUXER %s available\n", name);
	}
	CHECK(std::strstr(avcodec_configuration(), "--disable-shared"));
	CHECK(std::strstr(avcodec_configuration(), "--enable-static"));
	CHECK(std::strstr(avcodec_configuration(), "--disable-decoders"));
}

static unsigned writePackets(AVCodecContext* codec, AVFormatContext* format, AVStream* stream) {
	unsigned count = 0;
	AVPacket* packet = av_packet_alloc();
	CHECK(packet);
	while (true) {
		const int result = avcodec_receive_packet(codec, packet);
		if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
		requireFFmpeg(result, "receive GIF packet");
		packet->stream_index = stream->index;
		av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
		requireFFmpeg(av_interleaved_write_frame(format, packet), "mux GIF packet");
		av_packet_unref(packet);
		++count;
	}
	av_packet_free(&packet);
	return count;
}

static void syntheticGif(const char* path) {
	const int width = 16, height = 16, frameCount = 4;
	AVFilterGraph* graph = avfilter_graph_alloc();
	CHECK(graph);
	AVFilterContext *source = nullptr, *sink = nullptr, *split = nullptr, *palette = nullptr, *apply = nullptr;
	char args[128];
	std::snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=1/10", width, height, AV_PIX_FMT_RGB24);
	requireFFmpeg(avfilter_graph_create_filter(&source, avfilter_get_by_name("buffer"), "input", args, nullptr, graph), "create buffer");
	requireFFmpeg(avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "output", nullptr, nullptr, graph), "create buffersink");
	requireFFmpeg(avfilter_graph_create_filter(&split, avfilter_get_by_name("split"), "split", nullptr, nullptr, graph), "create split");
	requireFFmpeg(avfilter_graph_create_filter(&palette, avfilter_get_by_name("palettegen"), "palette", "reserve_transparent=off", nullptr, graph), "create palettegen");
	requireFFmpeg(avfilter_graph_create_filter(&apply, avfilter_get_by_name("paletteuse"), "apply", "dither=none", nullptr, graph), "create paletteuse");
	requireFFmpeg(avfilter_link(source, 0, split, 0), "link buffer/split");
	requireFFmpeg(avfilter_link(split, 0, palette, 0), "link split/palettegen");
	requireFFmpeg(avfilter_link(split, 1, apply, 0), "link split/paletteuse");
	requireFFmpeg(avfilter_link(palette, 0, apply, 1), "link palettegen/paletteuse");
	requireFFmpeg(avfilter_link(apply, 0, sink, 0), "link paletteuse/buffersink");
	requireFFmpeg(avfilter_graph_config(graph, nullptr), "configure GIF palette graph");

	AVFormatContext* format = nullptr;
	requireFFmpeg(avformat_alloc_output_context2(&format, nullptr, "gif", path), "allocate GIF muxer");
	CHECK(format);
	const AVCodec* encoder = avcodec_find_encoder_by_name("gif");
	AVCodecContext* codec = avcodec_alloc_context3(encoder);
	CHECK(codec);
	codec->width = width;
	codec->height = height;
	codec->pix_fmt = AV_PIX_FMT_PAL8;
	codec->time_base = AVRational{1, 10};
	if (format->oformat->flags & AVFMT_GLOBALHEADER) codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	requireFFmpeg(avcodec_open2(codec, encoder, nullptr), "open GIF encoder");
	AVStream* stream = avformat_new_stream(format, nullptr);
	CHECK(stream);
	stream->time_base = codec->time_base;
	requireFFmpeg(avcodec_parameters_from_context(stream->codecpar, codec), "copy GIF stream parameters");
	requireFFmpeg(avio_open(&format->pb, path, AVIO_FLAG_WRITE), "open synthetic GIF output");
	requireFFmpeg(avformat_write_header(format, nullptr), "write GIF header");

	AVFrame* input = av_frame_alloc();
	AVFrame* output = av_frame_alloc();
	CHECK(input && output);
	input->width = width;
	input->height = height;
	input->format = AV_PIX_FMT_RGB24;
	requireFFmpeg(av_frame_get_buffer(input, 32), "allocate RGB frame");
	for (int frame = 0; frame < frameCount; ++frame) {
		requireFFmpeg(av_frame_make_writable(input), "reuse RGB frame");
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				uint8_t* pixel = input->data[0] + y * input->linesize[0] + x * 3;
				pixel[0] = (x * 16 + frame * 32) & 255;
				pixel[1] = y * 16;
				pixel[2] = frame * 64;
			}
		}
		input->pts = frame;
		requireFFmpeg(av_buffersrc_add_frame_flags(source, input, AV_BUFFERSRC_FLAG_KEEP_REF), "submit RGB frame");
	}
	requireFFmpeg(av_buffersrc_add_frame_flags(source, nullptr, 0), "finish RGB input");
	unsigned frames = 0, packets = 0;
	while (true) {
		const int result = av_buffersink_get_frame(sink, output);
		if (result == AVERROR_EOF) break;
		requireFFmpeg(result, "retrieve paletted frame");
		CHECK(output->format == AV_PIX_FMT_PAL8 && output->width == width && output->height == height);
		requireFFmpeg(avcodec_send_frame(codec, output), "encode paletted GIF frame");
		packets += writePackets(codec, format, stream);
		av_frame_unref(output);
		++frames;
	}
	CHECK(frames == frameCount);
	requireFFmpeg(avcodec_send_frame(codec, nullptr), "flush GIF encoder");
	packets += writePackets(codec, format, stream);
	CHECK(packets >= frameCount);
	requireFFmpeg(av_write_trailer(format), "write GIF trailer");
	requireFFmpeg(avio_closep(&format->pb), "close GIF output");
	av_frame_free(&input);
	av_frame_free(&output);
	avcodec_free_context(&codec);
	avformat_free_context(format);
	avfilter_graph_free(&graph);

	FILE* file = std::fopen(path, "rb");
	CHECK(file);
	char signature[6];
	CHECK(std::fread(signature, 1, sizeof(signature), file) == sizeof(signature));
	CHECK(!std::memcmp(signature, "GIF87a", 6) || !std::memcmp(signature, "GIF89a", 6));
	CHECK(!std::fseek(file, 0, SEEK_END));
	const long bytes = std::ftell(file);
	CHECK(bytes > 32);
	CHECK(!std::fseek(file, -1, SEEK_END));
	CHECK(std::fgetc(file) == 0x3B);
	CHECK(!std::fclose(file));
	std::printf("GIF synthetic RGB/palette/encode/mux: %u frames, %u packets, %ld bytes; signature/trailer valid\n", frames, packets, bytes);
}

int main(int argc, char** argv) {
	CHECK(argc == 2);
	capabilities();
	if (std::strcmp(argv[1], "--list-only")) syntheticGif(argv[1]);
	std::printf("PASS static recording dependency smoke (%u checks). No ROM, BIOS, save, GUI or decoder used.\n", checks);
	return 0;
}
