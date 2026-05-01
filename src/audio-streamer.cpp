#include "obs-audio-to-websocket/audio-streamer.hpp"
#include "obs-audio-to-websocket/settings-dialog.hpp"
#include <chrono>
#include <cmath>
#include <algorithm>
#include <util/platform.h>
#include <util/threading.h>
#include <util/config-file.h>
#include <obs-module.h>

#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(param) (void)param
#endif

namespace obs_audio_to_websocket {

AudioStreamer &AudioStreamer::Instance()
{
	// Thread-safe since C++11 (guaranteed by standard)
	static AudioStreamer instance;
	return instance;
}

AudioStreamer::AudioStreamer() : m_lastRateUpdate(std::chrono::steady_clock::now()) {}

AudioStreamer::~AudioStreamer()
{
	m_shuttingDown = true;
	Stop();
}

void AudioStreamer::Start()
{
	if (m_streaming)
		return;

	m_streaming = true;

	ConnectToWebSocket();
	AttachAudioSource();

	emit streamingStatusChanged(true);
}

void AudioStreamer::Stop()
{
	if (!m_streaming)
		return;

	m_streaming = false;

	DetachAudioSource();
	DisconnectFromWebSocket();

	emit streamingStatusChanged(false);
}

void AudioStreamer::SetAudioSource(const std::string &sourceName)
{
	std::lock_guard<std::recursive_mutex> lock(m_sourceMutex);

	if (m_audioSourceName == sourceName)
		return;

	bool wasStreaming = m_streaming.load();
	if (wasStreaming) {
		DetachAudioSource();
	}

	m_audioSourceName = sourceName;

	if (wasStreaming) {
		AttachAudioSource();
	}
}

void AudioStreamer::ShowSettings()
{
	if (!m_settingsDialog) {
		m_settingsDialog = std::make_unique<SettingsDialog>();
	}

	// Toggle visibility if already open
	if (m_settingsDialog->isVisible()) {
		m_settingsDialog->hide();
	} else {
		m_settingsDialog->show();
		m_settingsDialog->raise();
		m_settingsDialog->activateWindow();
	}
}

void AudioStreamer::LoadSettings()
{
	// Load from OBS user config
#if LIBOBS_API_MAJOR_VER >= 31
	config_t *config = obs_frontend_get_user_config();
#else
	config_t *config = obs_frontend_get_profile_config();
#endif

	const char *url = config_get_string(config, "AudioStreamer", "WebSocketUrl");
	if (url && strlen(url) > 0) {
		std::lock_guard<std::mutex> lock(m_urlMutex);
		m_wsUrl = url;
	}

	const char *source = config_get_string(config, "AudioStreamer", "AudioSource");
	if (source && strlen(source) > 0) {
		std::lock_guard<std::recursive_mutex> lock(m_sourceMutex);
		m_audioSourceName = source;
	}

	bool autoConnect = config_get_bool(config, "AudioStreamer", "AutoConnect");
	m_autoConnectEnabled.store(autoConnect);
}

void AudioStreamer::ConnectToWebSocket()
{
	if (!m_wsClient) {
		m_wsClient = std::make_shared<WebSocketPPClient>();

		m_wsClient->SetOnConnected([this]() { OnWebSocketConnected(); });
		m_wsClient->SetOnDisconnected([this]() { OnWebSocketDisconnected(); });
		m_wsClient->SetOnMessage([this](const std::string &msg) { OnWebSocketMessage(msg); });
		m_wsClient->SetOnError([this](const std::string &err) { OnWebSocketError(err); });
	}

	std::string url;
	{
		std::lock_guard<std::mutex> lock(m_urlMutex);
		url = m_wsUrl;
	}
	m_wsClient->Connect(url);
}

void AudioStreamer::DisconnectFromWebSocket()
{
	if (m_wsClient) {
		if (!m_rawPcmMode) {
			m_wsClient->SendControlMessage("stop");
		}
		m_wsClient->Disconnect();
	}
}

void AudioStreamer::AttachAudioSource()
{
	std::lock_guard<std::recursive_mutex> lock(m_sourceMutex);

	if (m_audioSourceName.empty()) {
		blog(LOG_WARNING, "[Audio to WebSocket] No audio source name specified");
		return;
	}

	// Check if already attached to the same source
	if (m_audioSource && m_audioSourceName == m_audioSource.get_name()) {
		return;
	}

	// Detach any existing source first
	DetachAudioSource();

	// Get the source by name
	m_audioSource = OBSSourceWrapper(m_audioSourceName);
	if (!m_audioSource) {
		blog(LOG_ERROR, "[Audio to WebSocket] Audio source '%s' not found", m_audioSourceName.c_str());
		emit errorOccurred(QString("Audio source not found"));
		// Stop streaming if source attachment fails
		if (m_streaming) {
			Stop();
		}
		return;
	}

	// Verify it's an audio source
	if (!m_audioSource.is_audio_source()) {
		blog(LOG_ERROR, "[Audio to WebSocket] Source '%s' is not an audio source", m_audioSourceName.c_str());
		emit errorOccurred(QString("Selected source is not an audio source"));
		m_audioSource.reset();
		// Stop streaming if source attachment fails
		if (m_streaming) {
			Stop();
		}
		return;
	}

	// Use OBS audio capture API with static callback
	obs_source_add_audio_capture_callback(m_audioSource.get(), AudioCaptureCallback, this);
}

void AudioStreamer::DetachAudioSource()
{
	std::lock_guard<std::recursive_mutex> lock(m_sourceMutex);

	if (m_audioSource) {
		// Remove audio capture callback
		obs_source_remove_audio_capture_callback(m_audioSource.get(), AudioCaptureCallback, this);

		// RAII wrapper will handle release
		m_audioSource.reset();
	}
}

void AudioStreamer::AudioCaptureCallback(void *param, obs_source_t *source, const struct audio_data *audio_data,
					 bool muted)
{
	auto *streamer = static_cast<AudioStreamer *>(param);
	streamer->ProcessAudioData(source, audio_data, muted);
}

void AudioStreamer::ProcessAudioData(obs_source_t *source, const struct audio_data *audio_data, bool muted)
{
	static bool first_call = true;
	static int callback_count = 0;
	callback_count++;

	first_call = false;

	bool is_connected = m_wsClient && m_wsClient->IsConnected();

	if (m_shuttingDown || !m_streaming || muted || !m_wsClient || !is_connected) {
		return;
	}

	const audio_output_info *aoi = audio_output_get_info(obs_get_audio());
	if (!aoi)
		return;

	// Verify audio format
	if (aoi->format != AUDIO_FORMAT_FLOAT_PLANAR) {
		static bool format_error_logged = false;
		if (!format_error_logged) {
			format_error_logged = true;
			blog(LOG_ERROR, "[Audio to WebSocket] Unexpected audio format: %d (expected FLOAT_PLANAR)",
			     aoi->format);
		}
		return;
	}

	uint32_t sample_rate = aoi->samples_per_sec;
	uint32_t channels = static_cast<uint32_t>(audio_output_get_channels(obs_get_audio()));

	// Validate channel count
	if (channels > 8) { // OBS max is 8 channels
		static bool channel_error_logged = false;
		if (!channel_error_logged) {
			channel_error_logged = true;
			blog(LOG_ERROR, "[Audio to WebSocket] Too many channels: %u (max 8)", channels);
		}
		return;
	}

	// Convert to 16-bit PCM if needed
	size_t frames = audio_data->frames;
	size_t sample_size = sizeof(int16_t);
	size_t data_size = frames * channels * sample_size;

	AudioChunk chunk;
	chunk.data.resize(data_size);
	chunk.timestamp = audio_data->timestamp;
	chunk.format = AudioFormat(sample_rate, channels, 16);
	chunk.sourceId = obs_source_get_name(source);
	chunk.sourceName = obs_source_get_name(source);

	// Copy and convert audio data to 16-bit signed PCM (little-endian)
	uint8_t *out_ptr = chunk.data.data();

	// Variables for audio level analysis
	float peak_level = 0.0f;
	float rms_sum = 0.0f;
	size_t total_samples = 0;

	// Process audio frame by frame (interleaved output)
	size_t out_idx = 0;
	for (size_t i = 0; i < frames; ++i) {
		for (size_t ch = 0; ch < channels; ++ch) {
			// Validate that this channel's data exists
			if (!audio_data->data[ch]) {
				blog(LOG_ERROR, "[Audio to WebSocket] Missing data for channel %zu", ch);
				return;
			}

			const float *in_ptr = reinterpret_cast<const float *>(audio_data->data[ch]);
			float sample = in_ptr[i];

			// Track audio levels
			float abs_sample = std::abs(sample);
			if (abs_sample > peak_level) {
				peak_level = abs_sample;
			}
			rms_sum += sample * sample;
			total_samples++;

			// Clamp to [-1, 1] range
			sample = (std::max)(-1.0f, (std::min)(1.0f, sample));
			// Convert to 16-bit signed PCM with proper rounding
			int16_t sample_16 = static_cast<int16_t>(std::round(sample * 32767.0f));

			// Write in little-endian format explicitly
			out_ptr[out_idx++] = sample_16 & 0xFF;        // Low byte
			out_ptr[out_idx++] = (sample_16 >> 8) & 0xFF; // High byte
		}
	}

	// Calculate RMS level
	float rms_level = 0.0f;
	if (total_samples > 0) {
		rms_level = std::sqrt(rms_sum / total_samples);
	}

	// Log audio format info only once at startup
	static bool format_logged = false;
	if (!format_logged) {
		format_logged = true;
		blog(LOG_INFO, "[Audio to WebSocket] Streaming %u Hz, %u ch, 16-bit PCM (LE)", sample_rate, channels);
		blog(LOG_INFO, "[Audio to WebSocket] Source: %s, Format: FLOAT_PLANAR", obs_source_get_name(source));
		blog(LOG_INFO, "[Audio to WebSocket] Frame size: %zu samples, Buffer: %.1fms", frames,
		     (frames * 1000.0f) / sample_rate);

		// Log first few samples for debugging (only once)
		if (frames > 0 && channels > 0 && audio_data->data[0]) {
			const float *first_channel = reinterpret_cast<const float *>(audio_data->data[0]);
			blog(LOG_INFO, "[Audio to WebSocket] First 5 samples (ch0): %.4f %.4f %.4f %.4f %.4f",
			     first_channel[0], first_channel[1], first_channel[2], first_channel[3], first_channel[4]);
		}
	}

	// Only warn about silence, don't log normal levels
	static int silence_counter = 0;
	if (peak_level < 0.0001f) { // Essentially silence (-80 dB)
		silence_counter++;
		if (silence_counter == 500) { // After ~10 seconds at 48kHz
			blog(LOG_WARNING, "[Audio to WebSocket] No audio detected - check source");
		}
	} else {
		silence_counter = 0;
	}

	if (m_wsClient) {
		if (m_rawPcmMode) {
			// Raw PCM mode: downmix to mono + resample to 16kHz for WhisperLiveKit
			// Work in float space throughout to avoid quantization artifacts

			// Step 1: Downmix to mono float by averaging all channels
			std::vector<float> mono_float(frames);
			for (size_t i = 0; i < frames; ++i) {
				float sum = 0.0f;
				for (size_t ch = 0; ch < channels; ++ch) {
					const float *in_ptr = reinterpret_cast<const float *>(audio_data->data[ch]);
					sum += in_ptr[i];
				}
				mono_float[i] = sum / static_cast<float>(channels);
			}

			// Step 2: Resample from source rate to 16kHz
			// Use averaging filter to prevent aliasing (box filter over each output sample's span)
			constexpr uint32_t target_rate = 16000;
			std::vector<int16_t> output;

			if (sample_rate != target_rate && sample_rate > target_rate) {
				double ratio = static_cast<double>(sample_rate) / target_rate;
				size_t out_frames = static_cast<size_t>(frames / ratio);
				output.resize(out_frames);

				for (size_t i = 0; i < out_frames; ++i) {
					// Average all source samples that map to this output sample
					size_t src_start = static_cast<size_t>(i * ratio);
					size_t src_end = static_cast<size_t>((i + 1) * ratio);
					if (src_end > frames)
						src_end = frames;
					if (src_start >= src_end)
						src_start = src_end > 0 ? src_end - 1 : 0;

					float sum = 0.0f;
					for (size_t j = src_start; j < src_end; ++j) {
						sum += mono_float[j];
					}
					float avg = sum / static_cast<float>(src_end - src_start);
					avg = (std::max)(-1.0f, (std::min)(1.0f, avg));
					output[i] = static_cast<int16_t>(std::round(avg * 32767.0f));
				}
			} else {
				// Already at target rate or below, just quantize
				output.resize(frames);
				for (size_t i = 0; i < frames; ++i) {
					float s = (std::max)(-1.0f, (std::min)(1.0f, mono_float[i]));
					output[i] = static_cast<int16_t>(std::round(s * 32767.0f));
				}
			}

			m_wsClient->SendRawAudioData(reinterpret_cast<const uint8_t *>(output.data()),
						     output.size() * sizeof(int16_t));
			UpdateDataRate(output.size() * sizeof(int16_t));
		} else {
			m_wsClient->SendAudioData(chunk);
			UpdateDataRate(data_size);
		}
	} else {
		UpdateDataRate(data_size);
	}
}

void AudioStreamer::OnWebSocketConnected()
{
	if (!m_rawPcmMode && m_wsClient) {
		m_wsClient->SendControlMessage("start");
	}
	emit connectionStatusChanged(true);
}

void AudioStreamer::OnWebSocketDisconnected()
{
	emit connectionStatusChanged(false);
}

void AudioStreamer::OnWebSocketMessage(const std::string &message)
{
	UNUSED_PARAMETER(message);
	// Handle status messages from server
	try {
		// Parse JSON status message if needed
	} catch (...) {
		// Ignore parse errors
	}
}

void AudioStreamer::OnWebSocketError(const std::string &error)
{
	emit errorOccurred(QString::fromStdString(error));

	// Stop streaming if connection permanently failed
	if (error.find("Max reconnection attempts exceeded") != std::string::npos) {
		blog(LOG_ERROR, "[Audio to WebSocket] Connection permanently failed, stopping stream");
		Stop();
	}
}

void AudioStreamer::UpdateDataRate(size_t bytes)
{
	std::lock_guard<std::mutex> lock(m_rateMutex);

	m_bytesSinceLastUpdate += bytes;

	auto now = std::chrono::steady_clock::now();
	auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastRateUpdate);

	if (elapsed.count() >= 1000) {                                          // Update every second
		double kbps = (m_bytesSinceLastUpdate * 8.0) / elapsed.count(); // Convert to kilobits per second
		m_dataRate = kbps;

		m_bytesSinceLastUpdate = 0;
		m_lastRateUpdate = now;

		emit dataRateChanged(kbps);
	}
}

} // namespace obs_audio_to_websocket
