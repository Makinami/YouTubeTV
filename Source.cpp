extern "C" {
	#include <libavcodec/avcodec.h>
	#include <libavformat/avformat.h>
	#include <libswscale/swscale.h>
	#include <libavutil/imgutils.h>
}

#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "swscale.lib")
#pragma comment(lib, "avutil.lib")

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#undef main

#include <stdio.h>

void SaveFrame(AVFrame* pFrame, int width, int height, int iFrame) {
	FILE* pFile;
	char szFilename[32];
	int  y;

	// Open file
	sprintf_s(szFilename, "frame%d.ppm", iFrame);
	fopen_s(&pFile, szFilename, "wb");
	if (pFile == NULL)
		return;

	// Write header
	fprintf(pFile, "P6\n%d %d\n255\n", width, height);

	// Write pixel data
	for (y = 0; y < height; y++)
		fwrite(pFrame->data[0] + y * pFrame->linesize[0], 1, width * 3, pFile);

	// Close file
	fclose(pFile);
}

// https://blogs.gentoo.org/lu_zero/2016/03/29/new-avcodec-api/
// The flush packet is a non-NULL packet with size 0 and data NULL
int decode(AVCodecContext* avctx, AVFrame* frame, int* got_frame, AVPacket* pkt)
{
	int ret;

	*got_frame = 0;

	if (pkt) {
		ret = avcodec_send_packet(avctx, pkt);
		// In particular, we don't expect AVERROR(EAGAIN), because we read all
		// decoded frames with avcodec_receive_frame() until done.
		if (ret < 0)
			return ret == AVERROR_EOF ? 0 : ret;
	}

	ret = avcodec_receive_frame(avctx, frame);
	if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
		return ret;
	if (ret >= 0)
		*got_frame = 1;

	return 0;
}

int main(int argc, char* argv[])
{
	if (argc < 2) {
		fprintf(stderr, "Usage: test <file>\n");
		return -1;
	}

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
	{
		fprintf(stderr, "Could not initialize SDL- %s\n", SDL_GetError());
		return -1;
	}

	AVFormatContext* pFormatCtx = nullptr;

	// Open video file
	if (avformat_open_input(&pFormatCtx, argv[1], nullptr, 0) != 0)
	{
		fprintf(stderr, "Couldn't open file.");
		return 1;
	}

	// Retrieve stream information
	if (avformat_find_stream_info(pFormatCtx, nullptr) < 0)
	{
		fprintf(stderr, "Couldn't find stream info.");
		return 1;
	}

	// Dump information about file onto standard error
	av_dump_format(pFormatCtx, 0, argv[1], 0);

	auto videoStream = -1;
	for (auto i = 0; i < pFormatCtx->nb_streams; ++i)
	{
		if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			videoStream = i;
			break;
		}
	}

	if (videoStream == -1)
	{
		fprintf(stderr, "Couldn't find video stream.");
		return -1;
	}

	auto pCodecParams = pFormatCtx->streams[videoStream]->codecpar;

	auto pCodec = avcodec_find_decoder(pCodecParams->codec_id);
	if (pCodec == nullptr)
	{
		fprintf(stderr, "Unsupported coded.");
		return -1;
	}

	// Copy context
	auto pCodecCtx = avcodec_alloc_context3(pCodec);
	if (avcodec_parameters_to_context(pCodecCtx, pCodecParams) != 0)
	{
		fprintf(stderr, "Couldn't copy codec context.");
		return -1;
	}

	// Open codec
	if (avcodec_open2(pCodecCtx, pCodec, nullptr) < 0)
	{
		fprintf(stderr, "Couldn't open codec.");
		return -1;
	}

	// Allocate video frames
	auto pFrame = av_frame_alloc();
	auto pFrameRGB = av_frame_alloc();
	if (pFrameRGB == nullptr || pFrame == nullptr)
	{
		fprintf(stderr, "Couldn't allocate frames.");
		return -1;
	}

	// Determine  required buffer size and allocate buffer
	auto numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height, 32);
	auto buffer = static_cast<uint8_t*>(av_malloc(numBytes * sizeof(uint8_t)));

	// Assign appropriate parts of buffer to image planes in pFrameRGB
	// Note that pFrameRGB is an AVFrame, but AVFrame is a superset of AVPicture
	//avpicture_fill(static_cast<AVPicture*>(pFrameRGB), buffer, AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height);
	av_image_fill_arrays(pFrameRGB->data, pFrameRGB->linesize, buffer, AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height, 32);

	auto window = SDL_CreateWindow("YouTubeTV", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, pCodecCtx->width, pCodecCtx->height, SDL_WINDOW_SHOWN);
	if (!window)
	{
		fprintf(stderr, "SDL: could not create window - exiting\n");
		return -1;
	}

	auto renderer = SDL_CreateRenderer(window, -1, 0);
	if (!renderer)
	{
		fprintf(stderr, "SDL: could not create renderer - exiting\n");
	}

	auto bmp = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STATIC, pCodecCtx->width, pCodecCtx->height);

	SwsContext* sws_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,
		pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);

	AVPacket packet;
	SDL_Event event;

	int frameFinnished = 0;
	int i = 0;
	while (av_read_frame(pFormatCtx, &packet) >= 0)
	{
		// Is this a packet from the video stream?
		if (packet.stream_index == videoStream)
		{
			// Decode video frame
			decode(pCodecCtx, pFrame, &frameFinnished, &packet);

			// Did we get a video frame?
			if (frameFinnished)
			{
				SDL_UpdateYUVTexture(bmp, nullptr, pFrame->data[0], pFrame->linesize[0],
					pFrame->data[1], pFrame->linesize[1], pFrame->data[2], pFrame->linesize[2]);

				SDL_RenderCopy(renderer, bmp, nullptr, nullptr);
				SDL_RenderPresent(renderer);
				//// Convert the image from its native format to RGB
				//sws_scale(sws_ctx, static_cast<uint8_t const* const*>(pFrame->data), pFrame->linesize, 0,
				//	pCodecCtx->height, pFrameRGB->data, pFrameRGB->linesize);

				//// Save the frame to disk
				//if (++i < 25)
				//{
				//	SaveFrame(pFrameRGB, pCodecCtx->width, pCodecCtx->height, i);
				//}
			}
		}

		// Free the packet that was allocated by av_read_frame
		av_packet_unref(&packet);

		SDL_PollEvent(&event);
		switch (event.type)
		{
		case SDL_QUIT:
			SDL_Quit();
			return 0;
			break;
		default:
			break;
		}
	}

	// Free the RGB image
	av_free(buffer);
	av_free(pFrameRGB);

	// Free the YUV frame
	av_free(pFrame);

	// Close the codecs
	avcodec_close(pCodecCtx);

	// Close the video file
	avformat_close_input(&pFormatCtx);

	return 0;
}