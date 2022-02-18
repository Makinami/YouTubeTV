#include "pch.h"

#include "YouTubeVideo.h"

#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "swscale.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "swresample.lib")

#include <nlohmann/json.hpp>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif // _WIN32

using namespace std;
using namespace std::chrono_literals;
using json = nlohmann::json;

template<typename ... Args>
unique_ptr<SwrContext> make_swr_context(Args&&... args)
{
	auto swr = unique_ptr<SwrContext>(swr_alloc_set_opts(forward<Args>(args)...));

	if (!swr || swr_init(swr.get()) < 0)
		return nullptr;

	return swr;
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

	av_packet_unref(pkt);

	ret = avcodec_receive_frame(avctx, frame);
	if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
		return ret;
	if (ret >= 0)
		*got_frame = 1;

	return 0;
}

VideoStream::VideoStream(const string& _url, GuardedRenderer& _renderer, const Clock& _clock)
	: MediaStream{ _url, _clock }, renderer { _renderer }
{
	auto [lc, renderer_ptr] = renderer.get_renderer();
	current_frame = unique_ptr<SDL_Texture>{ SDL_CreateTexture(renderer_ptr, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STATIC, codec_ctx->width, codec_ctx->height) };
	back_buffer = unique_ptr<SDL_Texture>{ SDL_CreateTexture(renderer_ptr, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STATIC, codec_ctx->width, codec_ctx->height) };
}

void VideoStream::start()
{
	if (!decode_thread.joinable())
	{
		decode_thread = jthread([=](stop_token st) {
			while (!st.stop_requested())
			{
				if (seek_requested)
				{
					auto new_dts = new_time.count() / timebase;
					auto flags = 0;
					if (new_dts < working_frame->pts)
						flags |= AVSEEK_FLAG_BACKWARD;
					avformat_seek_file(format_ctx.get(), stream_index, INT64_MIN, static_cast<int64_t>(new_dts), INT64_MAX, flags);
					avcodec_flush_buffers(codec_ctx.get());
					seek_requested = false;

				}

				if (paused)
				{
					unique_lock<mutex> lc{ continue_mtx };
					continue_cv.wait(lc);
					continue;
				}

				decode_frame();
			}
		});
	}
}

void VideoStream::stop()
{
	if (decode_thread.joinable())
	{
		decode_thread.request_stop();
		continue_cv.notify_one();
		decode_thread.join();
	}
}

void VideoStream::pause()
{
	paused = true;
}

void VideoStream::unpause()
{
	paused = false;
	continue_cv.notify_one();
}

void VideoStream::decode_frame()
{
	AVPacket packet;
	int frameFinnished = 0;

	if (av_read_frame(format_ctx.get(), &packet) < 0)
	{
		stop();
		return;
	}

	if (packet.stream_index == stream_index)
	{
		decode(codec_ctx.get(), working_frame.get(), &frameFinnished, &packet);

		if (frameFinnished)
		{
			auto [rlc, renderer_ptr] = renderer.get_renderer();
			SDL_UpdateYUVTexture(back_buffer.get(), nullptr, working_frame->data[0], working_frame->linesize[0],
				working_frame->data[1], working_frame->linesize[1], working_frame->data[2], working_frame->linesize[2]);
			rlc.unlock();

			chrono::duration<double> frame_timestamp{ working_frame->pts * timebase };
			auto current_time = clock.time();
			auto diff = (frame_timestamp - current_time).count();
			if (frame_timestamp > current_time && !seek_requested)
			{
				this_thread::sleep_for(frame_timestamp - current_time);
			}

			lock_guard<mutex> lc{ frame_mtx };
			swap(back_buffer, current_frame);
		}
	}
}

void sdl_callback(void* ptr, Uint8* stream, int len)
{
	AudioStream* as = reinterpret_cast<AudioStream*>(ptr);

	while (len > 0)
	{
		if (as->seek_requested)
		{
			auto new_dts = as->new_time.count() / as->timebase;
			auto flags = 0;
			if (new_dts < as->working_frame->pts)
				flags |= AVSEEK_FLAG_BACKWARD;
			avformat_seek_file(as->format_ctx.get(), as->stream_index, INT64_MIN, static_cast<int64_t>(new_dts), INT64_MAX, flags);
			avcodec_flush_buffers(as->codec_ctx.get());
			as->seek_requested = false;

		}

		if (as->paused)
		{
			memset(stream, 0, len);
			return;
		}

		if (as->buffer_index >= as->buffer_size)
		{
			if (!as->decode_frame())
				continue;
		}

		auto data_size = min(as->buffer_size - as->buffer_index, len);

		memcpy(stream, as->audio_buffer.data() + as->buffer_index, data_size);
		stream += data_size;
		len -= data_size;
		as->buffer_index += data_size;
	}
}

AudioStream::AudioStream(const string& _url, const Clock& _clock)
	: MediaStream{ _url, _clock }
{
	SDL_AudioSpec wanted_spec, spec;
	wanted_spec.freq = codec_ctx->sample_rate;
	wanted_spec.format = AUDIO_F32SYS;
	wanted_spec.channels = codec_ctx->channels;
	wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
	wanted_spec.callback = sdl_callback;
	wanted_spec.userdata = this;

	device_id = SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_CHANNELS_CHANGE | SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
	if (device_id < 0)
		throw runtime_error("Could not open audio device");

	audio_tgt.fmt = AV_SAMPLE_FMT_FLT;
	audio_tgt.freq = spec.freq;
	audio_tgt.channel_layout = av_get_default_channel_layout(spec.channels);
	audio_tgt.channels = spec.channels;
	audio_tgt.frame_size = av_samples_get_buffer_size(nullptr, spec.channels, 1, audio_tgt.fmt, 1);
	audio_tgt.bytes_per_sec = av_samples_get_buffer_size(nullptr, spec.channels, audio_tgt.freq, audio_tgt.fmt, 1);

	audio_src = audio_tgt;

	difference_threshold = static_cast<double>(spec.size) / audio_tgt.bytes_per_sec;
}

void AudioStream::start()
{
	unpause();
}

void AudioStream::stop()
{
	pause();
}

void AudioStream::pause()
{
	SDL_PauseAudioDevice(device_id, 1);
}

void AudioStream::unpause()
{
	SDL_PauseAudioDevice(device_id, 0);
}

int AudioStream::decode_frame()
{
	AVPacket packet;
	int frameFinnished = 0;

	if (av_read_frame(format_ctx.get(), &packet) < 0)
	{
		return -1;
	}

	if (packet.stream_index == stream_index)
	{
		decode(codec_ctx.get(), working_frame.get(), &frameFinnished, &packet);

		if (!frameFinnished) return false;

		auto data_size = av_samples_get_buffer_size(nullptr, codec_ctx->channels,
			working_frame->nb_samples, codec_ctx->sample_fmt, 1);

		auto dec_channel_layout =
			(working_frame->channel_layout && working_frame->channels == av_get_channel_layout_nb_channels(working_frame->channel_layout)) ?
			working_frame->channel_layout : av_get_default_channel_layout(working_frame->channels);

		auto wanted_nb_samples = synchronize(working_frame->nb_samples);

		if (working_frame->format != audio_src.fmt ||
			dec_channel_layout != audio_src.channel_layout ||
			working_frame->sample_rate != audio_src.freq ||
			(wanted_nb_samples != working_frame->nb_samples && !swr_ctx))
		{
			swr_ctx = make_swr_context(nullptr, audio_tgt.channel_layout, audio_tgt.fmt, audio_tgt.freq,
				dec_channel_layout, static_cast<AVSampleFormat>(working_frame->format), working_frame->sample_rate, 0, nullptr);
			audio_src.channel_layout = dec_channel_layout;
			audio_src.channels = working_frame->channels;
			audio_src.freq = working_frame->sample_rate;
			audio_src.fmt = static_cast<AVSampleFormat>(working_frame->format);
		}

		if (swr_ctx)
		{
			const uint8_t** in = const_cast<const uint8_t**>(working_frame->extended_data);
			auto out = audio_buffer.data();
			int out_count = wanted_nb_samples * audio_tgt.freq / working_frame->sample_rate + 256;
			int out_size = av_samples_get_buffer_size(nullptr, audio_tgt.channels, out_count, audio_tgt.fmt, 0);
			if (out_size < 0)
			{
				fprintf(stderr, "av_samples_get_buffer_size() failed\n");
				return -1;
			}
			if (wanted_nb_samples != working_frame->nb_samples)
			{
				if (swr_set_compensation(swr_ctx.get(), (wanted_nb_samples - working_frame->nb_samples) * audio_tgt.freq / working_frame->sample_rate,
					wanted_nb_samples * audio_tgt.freq / working_frame->sample_rate) < 0)
				{
					fprintf(stderr, "swr_get_compensation() failed\n");
					return -1;
				}
			}
			// reallocated new buffer if current too small?
			auto len = swr_convert(swr_ctx.get(), &out, out_count, in, working_frame->nb_samples);
			if (len < 0)
			{
				fprintf(stderr, "swr_convert() failed\n");
				return -1;
			}
			if (len == out_count)
			{
				fprintf(stderr, "audio buffer is probably too small\n");
				if (swr_init(swr_ctx.get()) < 0) // reinit to remove buffered result
					swr_ctx = nullptr;
			}
			//is->audio_buf = out; // same thing
			buffer_size = len * audio_tgt.channels * av_get_bytes_per_sample(audio_tgt.fmt);
		}
		else
		{
			memcpy(audio_buffer.data(), working_frame->data[0], data_size);
			buffer_size = data_size;
		}

		buffer_index = 0;
	}

	return true;
}

int AudioStream::synchronize(int nb_samples)
{
	int wanted_nb_samples = nb_samples;

	int channels = format_ctx->streams[stream_index]->codecpar->channels;
	auto n = 4 * channels; // int or float

	double avg_diff;
	int min_nb_samples, max_nb_samples;

	chrono::duration<double> frame_timestamp{ working_frame->pts * timebase };
	auto current_time = clock.time();
	auto diff = chrono::duration_cast<std::chrono::duration<double>>(frame_timestamp - current_time).count();

	if (fabs(diff) < AV_NOSYNC_THRESHOLD)
	{
		// accumulate the diffs
		cummulative_difference = diff + average_difference_coef * cummulative_difference;
		if (average_differance_count < AUDIO_DIFF_AVG_NB)
		{
			++average_differance_count;
		}
		else
		{
			avg_diff = cummulative_difference * (1.0 - average_difference_coef);

			// Shrinking/expanding buffer code
			if (fabs(avg_diff) >= difference_threshold)
			{
				wanted_nb_samples = static_cast<int>(nb_samples + diff * format_ctx->streams[stream_index]->codecpar->sample_rate);
				min_nb_samples = static_cast<int>(nb_samples * ((100ll - SAMPLE_CORRECTION_PERCENT_MAX) / 100.));
				max_nb_samples = static_cast<int>(nb_samples * ((100ll + SAMPLE_CORRECTION_PERCENT_MAX) / 100.));

				wanted_nb_samples = std::clamp(wanted_nb_samples, min_nb_samples, max_nb_samples);
			}
		}
	}
	else
	{
		// difference is Too big; reset diff stuff
		average_differance_count = 0;
		cummulative_difference = 0.;
	}

	return wanted_nb_samples;
}

// From: https://stackoverflow.com/a/46348112
int SystemCapture(
	string         CmdLine,    //Command Line
	wstring         CmdRunDir,  //set to '.' for current directory
	string& ListStdOut, //Return List of StdOut
	string& ListStdErr, //Return List of StdErr
	uint32_t& RetCode)    //Return Exit Code
{
	int                  Success;
	SECURITY_ATTRIBUTES  security_attributes;
	HANDLE               stdout_rd = INVALID_HANDLE_VALUE;
	HANDLE               stdout_wr = INVALID_HANDLE_VALUE;
	HANDLE               stderr_rd = INVALID_HANDLE_VALUE;
	HANDLE               stderr_wr = INVALID_HANDLE_VALUE;
	PROCESS_INFORMATION  process_info;
	STARTUPINFO          startup_info;
	thread               stdout_thread;
	thread               stderr_thread;

	security_attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
	security_attributes.bInheritHandle = TRUE;
	security_attributes.lpSecurityDescriptor = nullptr;

	if (!CreatePipe(&stdout_rd, &stdout_wr, &security_attributes, 0) ||
		!SetHandleInformation(stdout_rd, HANDLE_FLAG_INHERIT, 0)) {
		return -1;
	}

	if (!CreatePipe(&stderr_rd, &stderr_wr, &security_attributes, 0) ||
		!SetHandleInformation(stderr_rd, HANDLE_FLAG_INHERIT, 0)) {
		if (stdout_rd != INVALID_HANDLE_VALUE) CloseHandle(stdout_rd);
		if (stdout_wr != INVALID_HANDLE_VALUE) CloseHandle(stdout_wr);
		return -2;
	}

	ZeroMemory(&process_info, sizeof(PROCESS_INFORMATION));
	ZeroMemory(&startup_info, sizeof(STARTUPINFO));

	startup_info.cb = sizeof(STARTUPINFO);
	startup_info.hStdInput = 0;
	startup_info.hStdOutput = stdout_wr;
	startup_info.hStdError = stderr_wr;

	if (stdout_rd || stderr_rd)
		startup_info.dwFlags |= STARTF_USESTDHANDLES;

	// Make a copy because CreateProcess needs to modify string buffer
	wstring CmdLineW(CmdLine.begin(), CmdLine.end());
	wchar_t      CmdLineStr[MAX_PATH];
	wcsncpy_s(CmdLineStr, MAX_PATH, CmdLineW.c_str(), CmdLineW.size());
	CmdLineStr[MAX_PATH - 1] = 0;

	Success = CreateProcess(
		nullptr,
		CmdLineStr,
		nullptr,
		nullptr,
		TRUE,
		0,
		nullptr,
		CmdRunDir.c_str(),
		&startup_info,
		&process_info
	);
	CloseHandle(stdout_wr);
	CloseHandle(stderr_wr);

	if (!Success) {
		CloseHandle(process_info.hProcess);
		CloseHandle(process_info.hThread);
		CloseHandle(stdout_rd);
		CloseHandle(stderr_rd);
		return -4;
	}
	else {
		CloseHandle(process_info.hThread);
	}

	if (stdout_rd) {
		stdout_thread = thread([&]() {
			DWORD  n;
			const size_t bufsize = 1000;
			char         buffer[bufsize];
			for (;;) {
				n = 0;
				int Success = ReadFile(
					stdout_rd,
					buffer,
					(DWORD)bufsize,
					&n,
					nullptr
				);
				if (!Success || n == 0)
					break;
				string s(buffer, n);
				ListStdOut += s;
			}
		});
	}

	if (stderr_rd) {
		stderr_thread = thread([&]() {
			DWORD        n;
			const size_t bufsize = 1000;
			char         buffer[bufsize];
			for (;;) {
				n = 0;
				int Success = ReadFile(
					stderr_rd,
					buffer,
					(DWORD)bufsize,
					&n,
					nullptr
				);
				if (!Success || n == 0)
					break;
				string s(buffer, n);
				ListStdOut += s;
			}
		});
	}

	WaitForSingleObject(process_info.hProcess, INFINITE);
	if (!GetExitCodeProcess(process_info.hProcess, (DWORD*)&RetCode))
		RetCode = -1;

	CloseHandle(process_info.hProcess);

	if (stdout_thread.joinable())
		stdout_thread.join();

	if (stderr_thread.joinable())
		stderr_thread.join();

	CloseHandle(stdout_rd);
	CloseHandle(stderr_rd);

	return 0;
}

YouTubeVideo::YouTubeVideo(string id, GuardedRenderer& renderer, int media_type)
{
	string out;
	string err;
	uint32_t code;
	string cmd = ".\\youtube-dl.exe -J https://www.youtube.com/watch?v="s + id;

	SystemCapture(cmd, L".", out, err, code);

	auto media_details = json::parse(out);

	if (media_type & Video)
	{
		auto video_format = find_if(media_details["requested_formats"].begin(), media_details["requested_formats"].end(), [](const auto& format) {
			return format["vcodec"].get<string>() != "none";
		});
		if (video_format != media_details["requested_formats"].end())
			video_stream = make_unique<VideoStream>((*video_format)["url"].get<string>(), renderer, clock);
			//video_stream = make_unique<VideoStream>("video.mp4", renderer, clock);
	}

	if (media_type & Audio)
	{
		auto audio_format = find_if(media_details["requested_formats"].begin(), media_details["requested_formats"].end(), [](const auto& format) {
			return format["acodec"].get<string>() != "none";
		});
		if (audio_format != media_details["requested_formats"].end())
			audio_stream = make_unique<AudioStream>((*audio_format)["url"].get<string>(), clock);
		//audio_stream = make_unique<AudioStream>("audio.webm", clock);
	}
}

void YouTubeVideo::start()
{
	if (video_stream)
		video_stream->start();
	if (audio_stream)
		audio_stream->start();
	clock.unpause();
	paused = false;
}

void YouTubeVideo::pause()
{
	if (video_stream)
		video_stream->pause();
	if (audio_stream)
		audio_stream->pause();
	clock.pause();
	paused = true;
}

void YouTubeVideo::unpause()
{
	if (video_stream)
		video_stream->unpause();
	if (audio_stream)
		audio_stream->unpause();
	clock.unpause();
	paused = false;
}

void YouTubeVideo::seek(std::chrono::duration<double> _new_time)
{
	if (video_stream)
		video_stream->seek(_new_time);
	if (audio_stream)
		audio_stream->seek(_new_time);
	clock.seek(_new_time);
}

auto YouTubeVideo::get_video_frame() -> decltype(std::declval<VideoStream>().get_frame())
{
	if (!video_stream)
		throw std::runtime_error("Media does not have active video stream");
	return video_stream->get_frame();
}
