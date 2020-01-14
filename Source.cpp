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

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

#undef main

#include <stdio.h>
#include <assert.h>

struct PacketQueue {
	AVPacketList* first_pkt, * last_pkt;
	int nb_packets;
	int size;
	SDL_mutex* mutex;
	SDL_cond* cond;
};

void packet_queue_init(PacketQueue* q)
{
	memset(q, 0, sizeof(PacketQueue));
	q->mutex = SDL_CreateMutex();
	q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue* q, AVPacket* pkt)
{
	if (av_packet_make_writable(pkt) < 0)
	{
		return -1;
	}
	auto pkt1 = static_cast<AVPacketList*>( av_malloc(sizeof(AVPacketList)) );
	if (!pkt1)
		return -1;
	pkt1->pkt = *pkt;
	pkt1->next = nullptr;

	SDL_LockMutex(q->mutex);

	if (!q->last_pkt)
		q->first_pkt = pkt1;
	else
		q->last_pkt->next = pkt1;
	q->last_pkt = pkt1;
	q->nb_packets++;
	q->size += pkt1->pkt.size;
	SDL_CondSignal(q->cond);

	SDL_UnlockMutex(q->mutex);
	return 0;
}

int quit = 0;

static int packet_queue_get(PacketQueue* q, AVPacket* pkt, int block)
{
	AVPacketList* pkt1;
	int ret;

	SDL_LockMutex(q->mutex);

	for (;;)
	{
		if (quit)
		{
			ret = -1;
			break;
		}

		pkt1 = q->first_pkt;
		if (pkt1)
		{
			q->first_pkt = pkt1->next;
			if (!q->first_pkt)
				q->last_pkt = nullptr;
			q->nb_packets--;
			q->size -= pkt1->pkt.size;
			*pkt = pkt1->pkt;
			av_free(pkt1);
			ret = 1;
			break;
		}
		else if (!block)
		{
			ret = 0;
			break;
		}
		else
		{
			SDL_CondWait(q->cond, q->mutex);
		}
	}
	SDL_UnlockMutex(q->mutex);
	return ret;
}

PacketQueue audioq;

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

	return ret;
}

int audio_decode_frame(AVCodecContext* aCodecCtx, uint8_t* audio_buf, int buf_size)
{
	static AVPacket pkt;
	static uint8_t* audio_pkt_data = nullptr;
	static int audio_pkt_samples = 0;
	static AVFrame frame;

	int len1, data_size = 0;

	for (;;)
	{
		while (audio_pkt_samples > 0)
		{
			int got_frame = 0;
			len1 = decode(aCodecCtx, &frame, &got_frame, &pkt);
			if (len1 < 0)
			{
				// If error, skip frame
				audio_pkt_samples = 0;
				break;
			}
			len1 = frame.linesize[0];
			audio_pkt_data += len1;
			audio_pkt_samples -= frame.nb_samples;
			if (got_frame)
			{
				if (av_sample_fmt_is_planar(aCodecCtx->sample_fmt) == 1)
				{
					// Test video has 2 channels decoded to planar as floats
					// SDL audio initiates with float32
					// Reference: https://gist.github.com/targodan/8cef8f2b682a30055aa7937060cd94b7 for supporting more formats
					float** fData = reinterpret_cast<float**>(frame.data);
					float* fBuf = reinterpret_cast<float*>(audio_buf);
					for (int i = 0; i < frame.nb_samples; ++i)
					{
						fBuf[2 * i] = fData[0][i];
						fBuf[2 * i + 1] = fData[1][i];
					}
					data_size = sizeof(float) * frame.nb_samples * 2;
				}
				else
				{
					data_size = av_samples_get_buffer_size(nullptr, aCodecCtx->channels,
						frame.nb_samples, aCodecCtx->sample_fmt, 1);
					assert(data_size <= buf_size);
					memcpy(audio_buf, frame.data[0], data_size);
				}
			}
			if (data_size <= 0)
			{
				// No data yet, get more frames
				continue;
			}
			// We have data, return it and come back for more later
			return data_size;
		}
		if (pkt.data)
			av_packet_unref(&pkt);

		if (quit)
			return -1;

		if (packet_queue_get(&audioq, &pkt, 1) < 0)
			return -1;

		audio_pkt_data = pkt.data;
		audio_pkt_samples = pkt.duration;
	}
}

FILE* fAudio;

void audio_callback(void* userdata, Uint8* stream, int len)
{
	auto aCodecCtx = static_cast<AVCodecContext*>(userdata);
	int len1, audio_size;

	static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
	static unsigned int audio_buf_size = 0;
	static unsigned int audio_buf_index = 0;

	while (len > 0)
	{
		if (audio_buf_index >= audio_buf_size)
		{
			// We have already sent all our data; get more
			audio_size = audio_decode_frame(aCodecCtx, audio_buf, sizeof(audio_buf));

			if (audio_size < 0)
			{
				// If error, output silence
				audio_buf_size = 1024;
				memset(audio_buf, 0, audio_buf_size);
			}
			else
			{
				audio_buf_size = audio_size;
			}
			audio_buf_index = 0;
		}
		len1 = audio_buf_size - audio_buf_index;
		if (len1 > len)
			len1 = len;
		memcpy(stream, audio_buf + audio_buf_index, len1);

		// dump to file
		fwrite(audio_buf + audio_buf_index, sizeof(uint8_t), len1, fAudio);

		len -= len1;
		stream += len1;
		audio_buf_index += len1;
	}
}

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

	fopen_s(&fAudio, "audio_dump.bin", "wb");

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
	auto audioStream = -1;
	for (auto i = 0; i < pFormatCtx->nb_streams; ++i)
	{
		if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && videoStream < 0)
		{
			videoStream = i;
		}

		if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audioStream < 0)
		{
			audioStream = i;
		}
	}

	if (videoStream == -1)
	{
		fprintf(stderr, "Couldn't find video stream.");
		return -1;
	}

	if (audioStream == -1)
	{
		fprintf(stderr, "Couldn't find audio stream.");
		return -1;
	}

	auto vCodecParams = pFormatCtx->streams[videoStream]->codecpar;

	auto vCodec = avcodec_find_decoder(vCodecParams->codec_id);
	if (vCodec == nullptr)
	{
		fprintf(stderr, "Unsupported video coded.");
		return -1;
	}

	// Copy context
	auto vCodecCtx = avcodec_alloc_context3(vCodec);
	if (avcodec_parameters_to_context(vCodecCtx, vCodecParams) != 0)
	{
		fprintf(stderr, "Couldn't copy video codec context.");
		return -1;
	}

	// Open codec
	if (avcodec_open2(vCodecCtx, vCodec, nullptr) < 0)
	{
		fprintf(stderr, "Couldn't open video codec.");
		return -1;
	}

	auto aCodecParams = pFormatCtx->streams[audioStream]->codecpar;

	auto aCodec = avcodec_find_decoder(aCodecParams->codec_id);
	if (!aCodec)
	{
		fprintf(stderr, "Unsupported audo codec.");
		return -1;
	}

	auto aCodecCtx = avcodec_alloc_context3(aCodec);
	if (avcodec_parameters_to_context(aCodecCtx, aCodecParams) != 0)
	{
		fprintf(stderr, "Couldn't copy audio codec context.");
		return -1;
	}

	// Set up SDL Audio
	SDL_AudioSpec wanted_spec, spec;
	wanted_spec.freq = aCodecCtx->sample_rate;
	wanted_spec.format = AUDIO_S16SYS;
	wanted_spec.channels = aCodecCtx->channels;
	wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
	wanted_spec.callback = audio_callback;
	wanted_spec.userdata = aCodecCtx;

	if (SDL_OpenAudio(&wanted_spec, &spec) < 0)
	{
		fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
		return -1;
	}

	packet_queue_init(&audioq);
	SDL_PauseAudio(0);

	// Open codec
	if (avcodec_open2(aCodecCtx, aCodec, nullptr) < 0)
	{
		fprintf(stderr, "Couldn't open audio codec.");
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
	auto numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, vCodecCtx->width, vCodecCtx->height, 32);
	auto buffer = static_cast<uint8_t*>(av_malloc(numBytes * sizeof(uint8_t)));

	// Assign appropriate parts of buffer to image planes in pFrameRGB
	// Note that pFrameRGB is an AVFrame, but AVFrame is a superset of AVPicture
	//avpicture_fill(static_cast<AVPicture*>(pFrameRGB), buffer, AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height);
	av_image_fill_arrays(pFrameRGB->data, pFrameRGB->linesize, buffer, AV_PIX_FMT_RGB24, vCodecCtx->width, vCodecCtx->height, 32);

	auto window = SDL_CreateWindow("YouTubeTV", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, vCodecCtx->width, vCodecCtx->height, SDL_WINDOW_SHOWN);
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

	auto bmp = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STATIC, vCodecCtx->width, vCodecCtx->height);

	SwsContext* sws_ctx = sws_getContext(vCodecCtx->width, vCodecCtx->height, vCodecCtx->pix_fmt,
		vCodecCtx->width, vCodecCtx->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);

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
			decode(vCodecCtx, pFrame, &frameFinnished, &packet);

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
				av_packet_unref(&packet);
			}
		}
		else if (packet.stream_index == audioStream)
		{
			packet_queue_put(&audioq, &packet);
		}
		else
		{
			av_packet_unref(&packet);
		}

		SDL_PollEvent(&event);
		switch (event.type)
		{
		case SDL_QUIT:
			quit = 1;
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
	avcodec_close(vCodecCtx);
	avcodec_close(aCodecCtx);

	// Close the video file
	avformat_close_input(&pFormatCtx);

	fclose(fAudio);

	return 0;
}