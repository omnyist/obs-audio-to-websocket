#pragma once

#include <QObject>
#include <memory>
#include <atomic>
#include <mutex>
#include <chrono>
#include <obs.h>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include "websocketpp-client.hpp"
#include "audio-format.hpp"
#include "obs-source-wrapper.hpp"

namespace obs_audio_to_websocket {

class SettingsDialog;

class AudioStreamer : public QObject {
	Q_OBJECT

public:
	static AudioStreamer &Instance();

	void Start();
	void Stop();
	bool IsStreaming() const { return m_streaming.load(); }

	void SetWebSocketUrl(const std::string &url)
	{
		std::lock_guard<std::mutex> lock(m_urlMutex);
		m_wsUrl = url;
	}
	std::string GetWebSocketUrl() const
	{
		std::lock_guard<std::mutex> lock(m_urlMutex);
		return m_wsUrl;
	}

	void SetAudioSource(const std::string &sourceName);
	std::string GetAudioSource() const { return m_audioSourceName; }

	void SetAutoConnectEnabled(bool enabled) { m_autoConnectEnabled.store(enabled); }
	bool IsAutoConnectEnabled() const { return m_autoConnectEnabled.load(); }

	void ShowSettings();
	void LoadSettings();

	double GetDataRate() const { return m_dataRate.load(); }
	bool IsConnected() const { return m_wsClient && m_wsClient->IsConnected(); }
	std::shared_ptr<WebSocketPPClient> GetWebSocketClient() const { return m_wsClient; }

	void ConnectToWebSocket();
	void DisconnectFromWebSocket();

signals:
	void connectionStatusChanged(bool connected);
	void streamingStatusChanged(bool streaming);
	void dataRateChanged(double kbps);
	void errorOccurred(const QString &error);

private:
	AudioStreamer();
	~AudioStreamer();
	AudioStreamer(const AudioStreamer &) = delete;
	AudioStreamer &operator=(const AudioStreamer &) = delete;

	static void AudioCaptureCallback(void *param, obs_source_t *source, const struct audio_data *audio_data,
					 bool muted);

	void ProcessAudioData(obs_source_t *source, const struct audio_data *audio_data, bool muted);
	void AttachAudioSource();
	void DetachAudioSource();

	void OnWebSocketConnected();
	void OnWebSocketDisconnected();
	void OnWebSocketMessage(const std::string &message);
	void OnWebSocketError(const std::string &error);

	void UpdateDataRate(size_t bytes);

	std::shared_ptr<WebSocketPPClient> m_wsClient;
	std::unique_ptr<SettingsDialog> m_settingsDialog;

	OBSSourceWrapper m_audioSource;
	std::string m_audioSourceName;
	std::string m_wsUrl = "ws://zelan:8765/asr";
	bool m_rawPcmMode{true}; // Send 16kHz mono PCM without header (for WhisperLiveKit)

	std::atomic<bool> m_streaming{false};
	std::atomic<bool> m_shuttingDown{false};
	std::atomic<bool> m_autoConnectEnabled{false};
	std::atomic<double> m_dataRate{0.0};

	std::recursive_mutex m_sourceMutex;
	mutable std::mutex m_urlMutex;

	// Data rate calculation
	std::chrono::steady_clock::time_point m_lastRateUpdate;
	size_t m_bytesSinceLastUpdate = 0;
	std::mutex m_rateMutex;
};

} // namespace obs_audio_to_websocket

// Module functions are declared in obs-module.h
