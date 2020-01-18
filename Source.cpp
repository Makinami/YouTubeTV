extern "C" {
	#include <libavcodec/avcodec.h>
	#include <libavformat/avformat.h>
	#include <libswscale/swscale.h>
	#include <libavutil/imgutils.h>
	#include <libavutil/avstring.h>
	#include <libavutil/time.h>
}

#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "swscale.lib")
#pragma comment(lib, "avutil.lib")

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)

#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 10.0

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

#define VIDEO_PICTURE_QUEUE_SIZE 1

#undef main

#include <stdio.h>
#include <assert.h>

//#define SURFACE

struct VideoPicture
{
	SDL_Texture* bmp;
	int width, height; // source height & width
	int allocated;
	double pts;
};

struct PacketQueue {
	AVPacketList* first_pkt, * last_pkt;
	int nb_packets;
	int size;
	SDL_mutex* mutex;
	SDL_cond* cond;
};

struct VideoState
{
	AVFormatContext* pFormatCtx;
	int videoStream, audioStream;

	double audio_clock;
	AVStream* audio_st;
	AVCodecContext* audio_ctx;
	PacketQueue audioq;
	uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
	unsigned int audio_buf_size;
	unsigned int audio_buf_index;
	AVFrame audio_frame;
	AVPacket audio_pkt;
	uint8_t* audio_pkt_data;
	int audio_pkt_size;
	AVStream* video_st;
	AVCodecContext* video_ctx;
	PacketQueue videoq;
	SwsContext* sws_ctx;

	double frame_timer;
	double frame_last_pts;
	double frame_last_delay;
	double video_clock; // pts of last decoded frame / predicted pts of next decoded frame

	VideoPicture pictq[VIDEO_PICTURE_QUEUE_SIZE];
	int pictq_size, pictq_rindex, pictq_windex;
	SDL_mutex* pictq_mutex;
	SDL_cond* pictq_cond;

	SDL_Thread* parse_tid;
	SDL_Thread* video_tid;

	char filename[1024];
	int quit;
};

SDL_Window* window;
SDL_Renderer* renderer;
SDL_mutex* renderer_mutex;

/* Since we only have one decoding thread, the Big Struct
   can be global in case we need it. */
VideoState* global_video_state;

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

int audio_decode_frame(VideoState* is, uint8_t* audio_buf, int buf_size, double* pts_ptr)
{
	int len1, data_size = 0;
	AVPacket* pkt = &is->audio_pkt;
	double pts;

	for (;;)
	{
		while (is->audio_pkt_size > 0)
		{
			int got_frame = 0;
			len1 = decode(is->audio_ctx, &is->audio_frame, &got_frame, pkt);
			if (len1 < 0)
			{
				// If error, skip frame
				is->audio_pkt_size= 0;
				break;
			}
			len1 = is->audio_frame.linesize[0];
			is->audio_pkt_data += len1;
			is->audio_pkt_size -= is->audio_frame.nb_samples;
			if (got_frame)
			{
				if (av_sample_fmt_is_planar(is->audio_ctx->sample_fmt) == 1)
				{
#ifdef SURFACE
					// Test video has 2 channels decoded to planar as floats
					// SDL audio initiates with float32
					// Reference: https://gist.github.com/targodan/8cef8f2b682a30055aa7937060cd94b7 for supporting more formats
					float** fData = reinterpret_cast<float**>(is->audio_frame.data);
					float* fBuf = reinterpret_cast<float*>(audio_buf);
					for (int i = 0; i < is->audio_frame.nb_samples; ++i)
					{
						fBuf[2 * i] = fData[0][i];
						fBuf[2 * i + 1] = fData[1][i];
					}
					data_size = sizeof(float) * is->audio_frame.nb_samples * 2;
#else
					// Test video has 2 channels decoded to planar as floats
					// Desktop output has 6 channels
					// SDL audio initiates with int32
					// Reference: https://gist.github.com/targodan/8cef8f2b682a30055aa7937060cd94b7 for supporting more formats
					float** fData = reinterpret_cast<float**>(is->audio_frame.data);
					float* fBuf = reinterpret_cast<float*>(audio_buf); // why not int?
					for (int i = 0; i < is->audio_frame.nb_samples; ++i)
					{
						fBuf[6 * i] = fData[0][i];
						fBuf[6 * i + 1] = fData[1][i];
						fBuf[6 * i + 2] = 0;
						fBuf[6 * i + 3] = 0;
						fBuf[6 * i + 4] = 0;
						fBuf[6 * i + 5] = 0;
					}
					data_size = sizeof(float) * is->audio_frame.nb_samples * 6;
#endif // SURFACE

				}
				else
				{
					data_size = av_samples_get_buffer_size(nullptr, is->audio_ctx->channels,
						is->audio_frame.nb_samples, is->audio_ctx->sample_fmt, 1);
					assert(data_size <= buf_size);
					memcpy(audio_buf, is->audio_frame.data[0], data_size);
				}
			}
			if (data_size <= 0)
			{
				// No data yet, get more frames
				continue;
			}

			// Keep audio_clock up-to-date
			pts = is->audio_clock;
			*pts_ptr = pts;
			is->audio_clock += is->audio_frame.nb_samples;

			// We have data, return it and come back for more later
			return data_size;
		}
		if (pkt->data)
			av_packet_unref(pkt);

		if (quit)
			return -1;

		if (packet_queue_get(&is->audioq, pkt, 1) < 0)
			return -1;

		is->audio_pkt_data = pkt->data;
		is->audio_pkt_size = pkt->size;
		// if update, update the audio clock w/pts
		if (pkt->pts != AV_NOPTS_VALUE)
			is->audio_clock = av_q2d(is->audio_st->time_base) * pkt->pts;
	}
}

double get_audio_clock(VideoState* is)
{
	double pts;
	int hw_buf_size, bytes_per_sec, n;

	pts = is->audio_clock;
	hw_buf_size = is->audio_buf_size - is->audio_buf_index;
	bytes_per_sec = 0;
	n = is->audio_st->codecpar->channels * 4; // float
	if (is->audio_st)
		bytes_per_sec = is->audio_st->codecpar->sample_rate * n;
	if (bytes_per_sec)
		pts -= (double)hw_buf_size / bytes_per_sec;
	return pts;
}

void audio_callback(void* userdata, Uint8* stream, int len)
{
	VideoState* is = (VideoState*)userdata;
	int len1, audio_size;
	double pts;

	static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
	static unsigned int audio_buf_size = 0;
	static unsigned int audio_buf_index = 0;

	while (len > 0)
	{
		if (audio_buf_index >= audio_buf_size)
		{
			// We have already sent all our data; get more
			audio_size = audio_decode_frame(is, audio_buf, sizeof(audio_buf), &pts);

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

void alloc_picture(void* userdata)
{
	VideoState* is = reinterpret_cast<VideoState*>(userdata);
	VideoPicture* vp;

	vp = &is->pictq[is->pictq_windex];
	if (vp->bmp)
		SDL_DestroyTexture(vp->bmp);

	SDL_LockMutex(renderer_mutex);
	vp->bmp = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STATIC, is->video_st->codecpar->width, is->video_st->codecpar->height);

	SDL_UnlockMutex(renderer_mutex);
	vp->width = is->video_st->codecpar->width;
	vp->height = is->video_st->codecpar->height;
	vp->allocated = 1;
}

int queue_picture(VideoState* is, AVFrame* pFrame, double pts)
{
	VideoPicture* vp;

	// wait until we have space for a new pic
	SDL_LockMutex(is->pictq_mutex);
	while (is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE && !is->quit)
		SDL_CondWait(is->pictq_cond, is->pictq_mutex);
	SDL_UnlockMutex(is->pictq_mutex);

	if (is->quit)
		return -1;

	// windex is set to 0 initially
	vp = &is->pictq[is->pictq_windex];

	// allocte or resize the buffer!
	if (!vp->bmp || vp->width != is->video_st->codecpar->width ||
		vp->height != is->video_st->codecpar->height)
	{
		vp->allocated = 0;
		alloc_picture(is);
		if (is->quit)
			return -1;
	}

	// We have a place to put our picture on the queue
	if (vp->bmp)
	{
		vp->pts = pts;
		SDL_UpdateYUVTexture(vp->bmp, nullptr, pFrame->data[0], pFrame->linesize[0],
			pFrame->data[1], pFrame->linesize[1], pFrame->data[2], pFrame->linesize[2]);
		
		if (++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE)
			is->pictq_windex = 0;

		SDL_LockMutex(is->pictq_mutex);
		is->pictq_size++;
		SDL_UnlockMutex(is->pictq_mutex);
	}

	return 0;
}

double synchronize_video(VideoState* is, AVFrame* src_frame, double pts)
{
	double frame_delay;

	if (pts != 0)
	{
		is->video_clock = pts;
	}
	else
	{
		pts = is->video_clock;
	}

	// update video clock
	frame_delay = av_q2d(is->video_st->time_base);
	// if we are repeating frame, adjust clock accordingly
	frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);
	is->video_clock += frame_delay;
	return pts;
}

int video_thread(void* arg)
{
	VideoState* is = reinterpret_cast<VideoState*>(arg);
	AVPacket pkt1, * packet = &pkt1;
	int frameFinished;
	AVFrame* pFrame;

	pFrame = av_frame_alloc();
	double pts;

	for (;;)
	{
		if (packet_queue_get(&is->videoq, packet, 1) < 0)
		{
			// means we quit getting packets
			break;
		}

		pts = 0;

		//Decode video frame
		decode(is->video_ctx, pFrame, &frameFinished, packet);

		if (packet->dts != AV_NOPTS_VALUE)
		{
			pts = pFrame->best_effort_timestamp;
		}
		else
		{
			pts = 0;
		}

		pts *= av_q2d(is->video_st->time_base);

		// Did we get a video frame?
		if (frameFinished)
		{
			pts = synchronize_video(is, pFrame, pts);
			if (queue_picture(is, pFrame, pts) < 0)
			{
				break;
			}
		}
		av_packet_unref(packet);
	}

	av_free(pFrame);

	return 0;

}

int stream_component_open(VideoState* is, int stream_index)
{
	AVFormatContext* pFormatCtx = is->pFormatCtx;
	AVCodecContext* codecCtx = nullptr;
	AVCodec* codec = nullptr;
	SDL_AudioSpec wanted_spec, spec;

	if (stream_index < 0 || stream_index >= pFormatCtx->nb_streams)
		return -1;

	auto vCodecParams = pFormatCtx->streams[stream_index]->codecpar;

	codec = avcodec_find_decoder(vCodecParams->codec_id);
	if (codec == nullptr)
	{
		fprintf(stderr, "Unsupported video coded.");
		return -1;
	}

	codecCtx = avcodec_alloc_context3(codec);
	if (avcodec_parameters_to_context(codecCtx, vCodecParams) != 0)
	{
		fprintf(stderr, "Couldn't copy video codec context.");
		return -1;
	}


	if (codecCtx->codec_type == AVMEDIA_TYPE_AUDIO)
	{
		SDL_AudioSpec wanted_spec, spec;
		wanted_spec.freq = codecCtx->sample_rate;
		wanted_spec.format = AUDIO_S16SYS;
		wanted_spec.channels = codecCtx->channels;
		wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
		wanted_spec.callback = audio_callback;
		wanted_spec.userdata = is;

		if (SDL_OpenAudio(&wanted_spec, &spec) < 0)
		{
			fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
			return -1;
		}
	}

	// Open codec
	if (avcodec_open2(codecCtx, codec, nullptr) < 0)
	{
		fprintf(stderr, "Couldn't open codec.");
		return -1;
	}

	switch (codecCtx->codec_type)
	{
	case AVMEDIA_TYPE_AUDIO:
		is->audioStream = stream_index;
		is->audio_st = pFormatCtx->streams[stream_index];
		is->audio_ctx = codecCtx;
		is->audio_buf_size = 0;
		is->audio_buf_index = 0;
		memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
		packet_queue_init(&is->audioq);
		SDL_PauseAudio(0);
		break;
	case AVMEDIA_TYPE_VIDEO:
		is->videoStream = stream_index;
		is->video_st = pFormatCtx->streams[stream_index];
		is->video_ctx = codecCtx;

		is->frame_timer = (double)av_gettime() / 1000000.0;
		is->frame_last_delay = 40e-3;

		packet_queue_init(&is->videoq);
		is->video_tid = SDL_CreateThread(video_thread, "video thread", is);
		is->sws_ctx = sws_getContext(codecCtx->width, codecCtx->height, codecCtx->pix_fmt,
			codecCtx->width, codecCtx->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
		break;
	default:
		break;
	}
}

int decode_thread(void* arg)
{
	VideoState* is = static_cast<VideoState*>(arg);
	AVFormatContext* pFormatCtx = nullptr;
	AVPacket pkt1, * packet = &pkt1;
	int video_index = -1;
	int audio_index = -1;
	int i;

	is->videoStream = -1;
	is->audioStream = -1;

	global_video_state = is;

	// Open video file
	if (avformat_open_input(&pFormatCtx, is->filename, nullptr, nullptr) != 0)
	{
		fprintf(stderr, "Couldn't open file");
		return -1;
	}

	is->pFormatCtx = pFormatCtx;

	// Retrieve stream information
	if (avformat_find_stream_info(pFormatCtx, nullptr) < 0)
	{
		fprintf(stderr, "Couldn't find stream information");
		return -1;
	}

	// Dump information about file onto standard error
	av_dump_format(pFormatCtx, 0, is->filename, 0);

	// Find the first video stream

	for (i = 0; i < pFormatCtx->nb_streams; ++i)
	{
		if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_index < 0)
		{
			video_index = i;
		}
		if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_index < 0)
		{
			audio_index = i;
		}
	}

	if (audio_index >= 0)
	{
		stream_component_open(is, audio_index);
	}
	if (video_index >= 0)
	{
		stream_component_open(is, video_index);
	}

	if (is->videoStream < 0 || is->audioStream < 0)
	{
		fprintf(stderr, "%s: coud not open codecs\n", is->filename);
		goto fail;
	}

	// main decode loop

	for (;;)
	{
		if (is->quit)
			break;

		// seed stuf goes here
		if (is->audioq.size > MAX_AUDIOQ_SIZE || is->videoq.size > MAX_VIDEOQ_SIZE)
		{
			SDL_Delay(10);
			continue;
		}

		if (av_read_frame(is->pFormatCtx, packet) < 0)
		{
			if (is->pFormatCtx->pb->error == 0)
			{
				SDL_Delay(100); // no error; wait for user input
				continue;
			}
			else
				break;
		}

		// Is this a packet from the video stream?
		if (packet->stream_index == is->videoStream)
		{
			packet_queue_put(&is->videoq, packet);
		}
		else if (packet->stream_index == is->audioStream)
		{
			packet_queue_put(&is->audioq, packet);
		}
		else
		{
			av_packet_unref(packet);
		}
	}

	// all done - wait for it
	while (!is->quit)
		SDL_Delay(100);

fail:
	if (1)
	{
		SDL_Event event;
		event.type = FF_QUIT_EVENT;
		event.user.data1 = is;
		SDL_PushEvent(&event);
	}

	return 0;
}

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void* opaque)
{
	SDL_Event event;
	event.type = FF_REFRESH_EVENT;
	event.user.data1 = opaque;
	SDL_PushEvent(&event);
	return 0;
}

// schedule a video refresh in 'delay' ms
static void schedule_refresh(VideoState* is, int delay)
{
	SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

void video_display(VideoState* is)
{
	SDL_Rect rect;
	VideoPicture* vp;
	float aspect_ratio;
	int w, h, x, y;
	int i;
	
	vp = &is->pictq[is->pictq_rindex];
	if (vp->bmp)
	{
		if (is->video_st->codecpar->sample_aspect_ratio.num == 0)
		{
			aspect_ratio = 0;
		}
		else
		{
			aspect_ratio = av_q2d(is->video_st->codecpar->sample_aspect_ratio) *
				is->video_st->codecpar->width / is->video_st->codecpar->height;
		}
		
		if (aspect_ratio <= 0.0)
		{
			aspect_ratio = (float)is->video_st->codecpar->width /
				(float)is->video_st->codecpar->height;
		}
		auto screen = SDL_GetWindowSurface(window);
		h = screen->h;
		w = ((int)rint(h * aspect_ratio)) & -4;
		if (w > screen->w)
		{
			w = screen->w;
			h = ((int)rint(w / aspect_ratio) & -4);
		}
		x = (screen->w - w) / 2;
		y = (screen->h - h) / 2;

		rect.x = x;
		rect.y = y;
		rect.w = w;
		rect.h = h;

		SDL_LockMutex(renderer_mutex);
		SDL_RenderCopy(renderer, vp->bmp, nullptr, &rect);
		SDL_RenderPresent(renderer);
		SDL_UnlockMutex(renderer_mutex);
	}
}

void video_refresh_timer(void* userdata)
{
	VideoState* is = reinterpret_cast<VideoState*>(userdata);
	VideoPicture* vp;
	double actual_delay, delay, sync_threshold, ref_clock, diff;

	if (is->video_st)
	{
		if (is->pictq_size == 0)
			schedule_refresh(is, 1);
		else
		{
			vp = &is->pictq[is->pictq_rindex];
			
			delay = vp->pts - is->frame_last_pts;
			if (delay <= 0 || delay >= 1)
			{
				// if incorrect delay, use previous one
				delay = is->frame_last_delay;
			}

			// save for next time
			is->frame_last_delay = delay;
			is->frame_last_pts = vp->pts;

			// update delay to sync to audio
			ref_clock = get_audio_clock(is);
			diff = vp->pts - ref_clock;

			/* Skip or repeat the frame. Take delay into account
				FFPlay still doesn't "know if this is the best guess." */
			sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;
			if (fabs(diff) < AV_NOSYNC_THRESHOLD)
			{
				if (diff <= -sync_threshold)
					delay = 0;
				else if (diff >= sync_threshold)
					delay = 2 * delay;
			}
			is->frame_timer += delay;

			// compute REAL delay
			actual_delay = is->frame_timer - (av_gettime() / 1000000.0);
			if (actual_delay < 0.010)
			{
				// Really it should skip the picture instead
				actual_delay = 0.010;
			}
			schedule_refresh(is, (int)(actual_delay * 1000 + 0.5));

			/* show the picture! */
			video_display(is);

			// update queue for next picture
			if (++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE)
				is->pictq_rindex = 0;

			SDL_LockMutex(is->pictq_mutex);
			is->pictq_size--;
			SDL_CondSignal(is->pictq_cond);
			SDL_UnlockMutex(is->pictq_mutex);
		}
	}
	else
	{
		schedule_refresh(is, 100);
	}
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

	SDL_Event event;

	renderer_mutex = SDL_CreateMutex();

	auto is = static_cast<VideoState*>(av_mallocz(sizeof(VideoState)));
	av_strlcpy(is->filename, argv[1], sizeof(is->filename));

	is->pictq_mutex = SDL_CreateMutex();
	is->pictq_cond = SDL_CreateCond();

	window = SDL_CreateWindow("YouTubeTV", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1024, 576, SDL_WINDOW_SHOWN);
	if (!window)
	{
		fprintf(stderr, "SDL: could not create window - exiting\n");
		return -1;
	}
	SDL_SetWindowResizable(window, SDL_TRUE);

	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
	renderer = SDL_CreateRenderer(window, -1, 0);
	if (!renderer)
	{
		fprintf(stderr, "SDL: could not create renderer - exiting\n");
	}

	auto bmp = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STATIC, 1024, 576);

	schedule_refresh(is, 40);

	is->parse_tid = SDL_CreateThread(decode_thread, "decode thread", is);
	if (!is->parse_tid)
	{
		fprintf(stderr, "Couldn't create decode thread");
		av_free(is);
		return -1;
	}

	while (true)
	{
		SDL_WaitEvent(&event);
		switch (event.type)
		{
		case FF_REFRESH_EVENT:
			video_refresh_timer(event.user.data1);
			break;
		case SDL_QUIT:
			quit = 1;
			SDL_Quit();
			return 0;
			break;
		default:
			break;
		}
	}

	return 0;
}