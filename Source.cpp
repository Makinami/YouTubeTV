extern "C" {
	#include <libavcodec/avcodec.h>
	#include <libavformat/avformat.h>
	#include <libswscale/swscale.h>
	#include <libavutil/imgutils.h>
	#include <libavutil/avstring.h>
	#include <libavutil/time.h>
	#include <libswresample/swresample.h>
}

#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "swscale.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "swresample.lib")

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)

#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 10.0

#define SAMPLE_CORRECTION_PERCENT_MAX 10
#define AUDIO_DIFF_AVG_NB 20

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

#define VIDEO_PICTURE_QUEUE_SIZE 1

#undef main

#include <stdio.h>
#include <assert.h>
#include <algorithm>

enum {
	AV_SYNC_AUDIO_MASTER,
	AV_SYNC_VIDEO_MASTER,
	AV_SYNC_EXTERNAL_MASTER
};

#define DEFAULT_AV_SYNC_TYPE AV_SYNC_EXTERNAL_MASTER

typedef struct AudioParams {
	int freq;
	int channels;
	int64_t channel_layout;
	enum AVSampleFormat fmt;
	int frame_size;
	int bytes_per_sec;
} AudioParams;

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

	int             av_sync_type;
	double          external_clock_start; /* external clock base */

	int seek_req;
	int seek_flags;
	int64_t seek_pos;

	double audio_clock;
	AVStream* audio_st;
	AVCodecContext* audio_ctx;
	PacketQueue audioq;
	uint8_t* audio_buf;
	unsigned int audio_buf_size;
	unsigned int audio_buf_index;
	AVFrame audio_frame;
	AVPacket audio_pkt;
	uint8_t* audio_pkt_data;
	int audio_pkt_size;
	int audio_hw_buffer_size;
	double audio_diff_cum;
	double audio_diff_avg_coef;
	double audio_diff_threshold;
	double audio_diff_avg_count;
	AVStream* video_st;
	AVCodecContext* video_ctx;
	PacketQueue videoq;
	SwsContext* sws_ctx;
	struct AudioParams audio_src;
	struct AudioParams audio_tgt;
	struct SwrContext* swr_ctx;


	double frame_timer;
	double frame_last_pts;
	double frame_last_delay;
	double video_clock; // pts of last decoded frame / predicted pts of next decoded frame
	double video_current_pts; // current displayed pts (different from video_clock if frame fifos are used)
	int64_t video_current_pts_time; // time (av_gettime) at which we updated video_current_pts - used to have running video pts


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

AVPacket flush_pkt;

int packet_queue_put(PacketQueue* q, AVPacket* pkt)
{
	if (pkt != &flush_pkt && av_packet_make_refcounted(pkt) < 0)
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

static void packet_queue_flush(PacketQueue* q)
{
	AVPacketList* pkt, * pkt1;

	SDL_LockMutex(q->mutex);
	for (pkt = q->first_pkt; pkt != nullptr; pkt = pkt1)
	{
		pkt1 = pkt->next;
		av_packet_unref(&pkt->pkt);
		av_freep(&pkt);
	}

	q->last_pkt = nullptr;
	q->first_pkt = nullptr;
	q->nb_packets = 0;
	q->size = 0;
	SDL_UnlockMutex(q->mutex);
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

double get_video_clock(VideoState* is)
{
	double delta;

	delta = (av_gettime() - is->video_current_pts_time) / 1000000.0;
	return is->video_current_pts + delta;
}

double get_external_clock(VideoState* is)
{
	return (av_gettime() - is->external_clock_start) / 1000000.0;
}

double get_master_clock(VideoState* is)
{
	if (is->av_sync_type == AV_SYNC_VIDEO_MASTER)
		return get_video_clock(is);
	else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER)
		return get_video_clock(is);
	else
		return get_external_clock(is);
}

int synchronize_audio(VideoState* is, int nb_samples)
{
	int wanted_nb_samples = nb_samples;
	int n;
	double ref_clock;

	int channels = is->audio_st->codecpar->channels;
	n = 4 * channels; // int or float

	if (is->av_sync_type != AV_SYNC_AUDIO_MASTER)
	{
		double diff, avg_diff;
		int min_nb_samples, max_nb_samples;

		ref_clock = get_master_clock(is);
		diff = get_audio_clock(is) - ref_clock;

		if (fabs(diff) < AV_NOSYNC_THRESHOLD)
		{
			// accumulate the diffs
			is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
			if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB)
			{
				is->audio_diff_avg_count++;
			}
			else
			{
				avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);

				// Shrinking/expanding buffer code
				if (fabs(avg_diff) >= is->audio_diff_threshold)
				{
					wanted_nb_samples = nb_samples + diff * is->audio_st->codecpar->sample_rate;
					min_nb_samples = nb_samples * ((100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100);
					max_nb_samples = nb_samples * ((100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100);

					wanted_nb_samples = std::clamp(wanted_nb_samples, min_nb_samples, max_nb_samples);
				}
			}
		}
		else
		{
			// difference is Too big; reset diff stuff
			is->audio_diff_avg_count = 0;
			is->audio_diff_cum = 0;
		}
	}

	return wanted_nb_samples;
}

int audio_decode_frame(VideoState* is, uint8_t* audio_buf, int buf_size)
{
	int len1, data_size = 0, resampled_data_size;
	AVPacket* pkt = &is->audio_pkt;
	int64_t dec_channel_layout;
	int wanted_nb_samples;

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
				data_size = av_samples_get_buffer_size(nullptr, is->audio_ctx->channels,
					is->audio_frame.nb_samples, is->audio_ctx->sample_fmt, 1);

				dec_channel_layout =
					(is->audio_frame.channel_layout && is->audio_frame.channels == av_get_channel_layout_nb_channels(is->audio_frame.channel_layout)) ?
					is->audio_frame.channel_layout : av_get_default_channel_layout(is->audio_frame.channels);

				wanted_nb_samples = synchronize_audio(is, is->audio_frame.nb_samples);

				if (is->audio_frame.format != is->audio_src.fmt ||
					dec_channel_layout != is->audio_src.channel_layout ||
					is->audio_frame.sample_rate != is->audio_src.freq ||
					(wanted_nb_samples != is->audio_frame.nb_samples && !is->swr_ctx))
				{
					swr_free(&is->swr_ctx);
					is->swr_ctx = swr_alloc_set_opts(nullptr, is->audio_tgt.channel_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
						dec_channel_layout, (AVSampleFormat)is->audio_frame.format, is->audio_frame.sample_rate,
						0, nullptr);
					if (!is->swr_ctx || swr_init(is->swr_ctx) < 0)
					{
						fprintf(stderr,
							"Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
							is->audio_frame.sample_rate, av_get_sample_fmt_name((AVSampleFormat)is->audio_frame.format), is->audio_frame.channels,
							is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.channels);
						swr_free(&is->swr_ctx);
					}
					is->audio_src.channel_layout = dec_channel_layout;
					is->audio_src.channels = is->audio_frame.channels;
					is->audio_src.freq = is->audio_frame.sample_rate;
					is->audio_src.fmt = (AVSampleFormat)is->audio_frame.format;
				}

				if (is->sws_ctx)
				{
					const uint8_t** in = (const uint8_t**)is->audio_frame.extended_data;
					auto out = &is->audio_buf;
					int out_count = (int64_t)wanted_nb_samples * is->audio_tgt.freq / is->audio_frame.sample_rate + 256;
					int out_size = av_samples_get_buffer_size(nullptr, is->audio_tgt.channels, out_count, is->audio_tgt.fmt, 0);
					int len2;
					if (out_size < 0)
					{
						fprintf(stderr, "av_samples_get_buffer_size() failed\n");
						return -1;
					}
					if (wanted_nb_samples != is->audio_frame.nb_samples)
					{
						if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - is->audio_frame.nb_samples) * is->audio_tgt.freq / is->audio_frame.sample_rate,
							wanted_nb_samples * is->audio_tgt.freq / is->audio_frame.sample_rate) < 0)
						{
							fprintf(stderr, "swr_get_compensation() failed\n");
							return -1;
						}
					}
					// reallocated new buffer if current too small?
					len2 = swr_convert(is->swr_ctx, out, out_count, in, is->audio_frame.nb_samples);
					if (len2 < 0)
					{
						fprintf(stderr, "swr_convert() failed\n");
						return -1;
					}
					if (len2 == out_count)
					{
						fprintf(stderr, "audio buffer is probably too small\n");
						if (swr_init(is->swr_ctx) < 0) // reinit to remove buffered result
							swr_free(&is->swr_ctx);
					}
					//is->audio_buf = out; // same thing
					resampled_data_size = len2 * is->audio_tgt.channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
				}
				else
				{
					//is->audio_buf = af->frame->data[0]; // audio_buf is an array not ptr
					memcpy(is->audio_buf, is->audio_frame.data[0], data_size);
					resampled_data_size = data_size;
				}

			}
			if (data_size <= 0)
			{
				// No data yet, get more frames
				continue;
			}

			// Keep audio_clock up-to-date
			is->audio_clock += is->audio_frame.nb_samples * (1.0 / is->audio_ctx->sample_rate);

			// We have data, return it and come back for more later
			return resampled_data_size;
		}
		if (pkt->data)
			av_packet_unref(pkt);

		if (quit)
			return -1;

		if (packet_queue_get(&is->audioq, pkt, 1) < 0)
			return -1;

		if (pkt->data == flush_pkt.data)
		{
			avcodec_flush_buffers(is->audio_ctx);
			continue;
		}

		is->audio_pkt_data = pkt->data;
		is->audio_pkt_size = pkt->size;
		// if update, update the audio clock w/pts
		if (pkt->pts != AV_NOPTS_VALUE)
			is->audio_clock = av_q2d(is->audio_st->time_base) * pkt->pts;
	}
}

void audio_callback(void* userdata, Uint8* stream, int len)
{
	VideoState* is = (VideoState*)userdata;
	int len1, audio_size;

	while (len > 0)
	{
		if (is->audio_buf_index >= is->audio_buf_size)
		{
			// We have already sent all our data; get more
			audio_size = audio_decode_frame(is, is->audio_buf, sizeof(is->audio_buf));

			if (audio_size < 0)
			{
				// If error, output silence
				is->audio_buf_size = 1024;
				memset(is->audio_buf, 0, is->audio_buf_size);
			}
			else
			{
				is->audio_buf_size = audio_size;
			}
			is->audio_buf_index = 0;
		}
		len1 = is->audio_buf_size - is->audio_buf_index;
		if (len1 > len)
			len1 = len;
		memcpy(stream, is->audio_buf + is->audio_buf_index, len1);

		len -= len1;
		stream += len1;
		is->audio_buf_index += len1;
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

		if (packet->data == flush_pkt.data) {
			avcodec_flush_buffers(is->video_ctx);
			continue;
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
	int dev;

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
		wanted_spec.format = AUDIO_F32SYS;
		wanted_spec.channels = codecCtx->channels;
		wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
		wanted_spec.callback = audio_callback;
		wanted_spec.userdata = is;

		if ((dev = SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_CHANNELS_CHANGE | SDL_AUDIO_ALLOW_FREQUENCY_CHANGE)) < 0)
		{
			fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
			return -1;
		}

		is->audio_tgt.fmt = AV_SAMPLE_FMT_FLT;
		is->audio_tgt.freq = spec.freq;
		is->audio_tgt.channel_layout = av_get_default_channel_layout(spec.channels);
		is->audio_tgt.channels = spec.channels;
		is->audio_tgt.frame_size = av_samples_get_buffer_size(nullptr, spec.channels, 1, is->audio_tgt.fmt, 1);
		is->audio_tgt.bytes_per_sec = av_samples_get_buffer_size(nullptr, spec.channels, is->audio_tgt.freq, is->audio_tgt.fmt, 1);
		if (is->audio_tgt.bytes_per_sec <= 0 || is->audio_tgt.frame_size <= 0) {
			fprintf(stderr, "av_samples_get_buffer_size failed\n");
			return -1;
		}

		is->audio_src = is->audio_tgt;
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
		SDL_PauseAudioDevice(dev, 0);
		break;
	case AVMEDIA_TYPE_VIDEO:
		is->videoStream = stream_index;
		is->video_st = pFormatCtx->streams[stream_index];
		is->video_ctx = codecCtx;

		is->frame_timer = (double)av_gettime() / 1000000.0;
		is->frame_last_delay = 40e-3;
		is->video_current_pts_time = av_gettime();

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

	is->external_clock_start = av_gettime();

	// main decode loop

	for (;;)
	{
		if (is->quit)
			break;

		// seek stuf goes here
		if (is->seek_req)
		{
			int stream_index = -1;
			int64_t seek_target = is->seek_pos;

			if (is->videoStream >= 0) stream_index = is->videoStream;
			else if (is->videoStream >= 0) stream_index = is->audioStream;

			if (stream_index >= 0)
			{
				seek_target = av_rescale_q(seek_target, AVRational{ 1, AV_TIME_BASE }, pFormatCtx->streams[stream_index]->time_base);
			}
			if (av_seek_frame(is->pFormatCtx, stream_index, seek_target, is->seek_flags) < 0)
			{
				fprintf(stderr, "%s: error while seeking\n", is->pFormatCtx->url);
			}
			else
			{
				if (is->audioStream >= 0)
				{
					packet_queue_flush(&is->audioq);
					packet_queue_put(&is->audioq, &flush_pkt);
				}
				if (is->videoStream >= 0)
				{
					packet_queue_flush(&is->videoq);
					packet_queue_put(&is->videoq, &flush_pkt);
				}
			}
			is->seek_req = 0;
		}

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

			is->video_current_pts = vp->pts;
			is->video_current_pts_time = av_gettime();
			
			delay = vp->pts - is->frame_last_pts;
			if (delay <= 0 || delay >= 1)
			{
				// if incorrect delay, use previous one
				delay = is->frame_last_delay;
			}

			// save for next time
			is->frame_last_delay = delay;
			is->frame_last_pts = vp->pts;

			// Update delay to sync to audio if not master source
			if (is->av_sync_type != AV_SYNC_VIDEO_MASTER)
			{
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

void stream_seek(VideoState* is, int64_t pos, int rel)
{
	if (!is->seek_req)
	{
		is->seek_pos = pos;
		is->seek_flags = rel < 0 ? AVSEEK_FLAG_BACKWARD : 0;
		is->seek_req = 1;
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
	is->audio_buf = new uint8_t[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
	av_strlcpy(is->filename, argv[1], sizeof(is->filename));

	is->pictq_mutex = SDL_CreateMutex();
	is->pictq_cond = SDL_CreateCond();
	is->av_sync_type = DEFAULT_AV_SYNC_TYPE;

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

	av_init_packet(&flush_pkt);
	flush_pkt.data = (uint8_t*)("FLUSH");

	while (true)
	{
		double incr, pos;

		SDL_WaitEvent(&event);
		switch (event.type)
		{
		case SDL_KEYDOWN:
			switch (event.key.keysym.sym)
			{
			case SDLK_LEFT:
				incr = -10;
				goto do_seek;
			case SDLK_RIGHT:
				incr = 10;
				goto do_seek;
			do_seek:
				if (global_video_state)
				{
					pos = get_master_clock(global_video_state);
					pos += incr;
					stream_seek(global_video_state, (int64_t)(pos * AV_TIME_BASE), incr);
				}
				break;
			default:
				break;
			}
			break;
		case FF_REFRESH_EVENT:
			video_refresh_timer(event.user.data1);
			break;
		case SDL_QUIT:
			quit = 1;
			/*
	   * If the video has finished playing, then both the picture and
	   * audio queues are waiting for more data.  Make them stop
	   * waiting and terminate normally.
	   */
			SDL_CondSignal(is->audioq.cond);
			SDL_CondSignal(is->videoq.cond);
			SDL_Quit();
			return 0;
			break;
		default:
			break;
		}
	}

	delete[] is->audio_buf;

	return 0;
}