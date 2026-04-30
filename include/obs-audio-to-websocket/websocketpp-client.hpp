#pragma once

// These macros are defined in CMakeLists.txt, don't redefine them here

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/client.hpp>
#include <websocketpp/connection.hpp>
#include <websocketpp/logger/levels.hpp>
#include <websocketpp/common/thread.hpp>
#include <websocketpp/common/memory.hpp>
#include <thread>
#include <mutex>
#include <functional>
#include <atomic>
#include <memory>
#include <string>
#include "audio-format.hpp"

namespace obs_audio_to_websocket {

// Using non-TLS client for simplicity (ws:// only) - matching obs-websocket
using client = websocketpp::client<websocketpp::config::asio>;
using message_ptr = websocketpp::config::asio::message_type::ptr;

class WebSocketPPClient : public std::enable_shared_from_this<WebSocketPPClient> {
public:
	using OnConnectedCallback = std::function<void()>;
	using OnDisconnectedCallback = std::function<void()>;
	using OnMessageCallback = std::function<void(const std::string &)>;
	using OnErrorCallback = std::function<void(const std::string &)>;

	WebSocketPPClient();
	virtual ~WebSocketPPClient();

	bool Connect(const std::string &uri);
	void Disconnect();
	bool IsConnected() const { return m_connected.load(); }

	void SendAudioData(const AudioChunk &chunk);
	void SendRawAudioData(const uint8_t *data, size_t size);
	void SendControlMessage(const std::string &type);

	void SetOnConnected(OnConnectedCallback cb) { m_onConnected = cb; }
	void SetOnDisconnected(OnDisconnectedCallback cb) { m_onDisconnected = cb; }
	void SetOnMessage(OnMessageCallback cb) { m_onMessage = cb; }
	void SetOnError(OnErrorCallback cb) { m_onError = cb; }

	void SetAutoReconnect(bool enable) { m_shouldReconnect = enable; }
	bool IsAutoReconnectEnabled() const { return m_shouldReconnect; }
	bool IsReconnecting() const { return m_reconnecting.load(); }
	int GetReconnectAttempts() const { return m_reconnectAttempts.load(); }

private:
	void Run();
	void OnOpen(websocketpp::connection_hdl hdl);
	void OnClose(websocketpp::connection_hdl hdl);
	void OnMessage(websocketpp::connection_hdl hdl, message_ptr msg);
	void OnFail(websocketpp::connection_hdl hdl);
	void ScheduleReconnect();
	void DoReconnect();

	client m_client;
	websocketpp::connection_hdl m_hdl;
	mutable std::mutex m_hdlMutex; // Protect m_hdl access
	std::thread m_thread;
	bool m_threadRunning{false};

	std::atomic<bool> m_connected{false};
	std::atomic<bool> m_running{false};
	std::atomic<bool> m_shouldReconnect{true};

	// Reconnection state
	std::thread m_reconnectThread;
	std::atomic<int> m_reconnectAttempts{0};
	std::atomic<bool> m_reconnecting{false};

	std::string m_uri;

	OnConnectedCallback m_onConnected;
	OnDisconnectedCallback m_onDisconnected;
	OnMessageCallback m_onMessage;
	OnErrorCallback m_onError;
};

} // namespace obs_audio_to_websocket
