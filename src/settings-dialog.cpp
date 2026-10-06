#include "obs-audio-to-websocket/settings-dialog.hpp"
#include "obs-audio-to-websocket/audio-streamer.hpp"
#include "obs-audio-to-websocket/websocketpp-client.hpp"
#include "obs-audio-to-websocket/obs-source-wrapper.hpp"
#include <chrono>
#include <util/config-file.h>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>
#include <QLabel>
#include <QProgressBar>
#include <QMessageBox>
#include <QUrl>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSignalBlocker>
#include <obs.h>
#include <obs-module.h>
#include <obs-frontend-api.h>

#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(param) (void)param
#endif

namespace obs_audio_to_websocket {

namespace {
QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}
} // namespace

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent), m_streamer(&AudioStreamer::Instance())
{
	setupUi();
	connectSignals();
	loadSettings();

	// Apply default microphone selection if no valid source was loaded
	if (m_audioSourceCombo->currentIndex() < 0 || m_audioSourceCombo->currentText().isEmpty()) {
		selectDefaultMicrophoneSource();
	}

	m_updateTimer = std::make_unique<QTimer>(this);
	connect(m_updateTimer.get(), &QTimer::timeout, this, &SettingsDialog::updateStatus);
	m_updateTimer->start(100); // Update every 100ms
}

SettingsDialog::~SettingsDialog()
{
	cleanupVolumeMeter();
}

void SettingsDialog::setupUi()
{
	setWindowTitle(T("AudioStreamerSettings"));
	setFixedSize(450, 440);

	auto *mainLayout = new QVBoxLayout(this);

	// Connection Settings Group
	auto *connectionGroup = new QGroupBox(T("WebSocketConnection"), this);
	auto *connectionLayout = new QGridLayout(connectionGroup);

	connectionLayout->addWidget(new QLabel(T("URL"), this), 0, 0);
	m_urlEdit = new QLineEdit(this);
	m_urlEdit->setPlaceholderText("ws://localhost:8889/audio");
	connectionLayout->addWidget(m_urlEdit, 0, 1, 1, 2);

	m_testButton = new QPushButton(T("TestConnection"), this);
	connectionLayout->addWidget(m_testButton, 0, 3);

	m_autoConnectCheckBox = new QCheckBox(T("AutoConnect"), this);
	connectionLayout->addWidget(m_autoConnectCheckBox, 1, 0, 1, 4);

	mainLayout->addWidget(connectionGroup);

	// Audio Settings Group
	auto *audioGroup = new QGroupBox(T("AudioSettings"), this);
	auto *audioLayout = new QGridLayout(audioGroup);

	audioLayout->addWidget(new QLabel(T("Source"), this), 0, 0);
	m_audioSourceCombo = new QComboBox(this);
	audioLayout->addWidget(m_audioSourceCombo, 0, 1, 1, 2);

	m_refreshButton = new QPushButton(T("Refresh"), this);
	m_refreshButton->setMaximumWidth(80);
	connect(m_refreshButton, &QPushButton::clicked, this, &SettingsDialog::populateAudioSources);
	audioLayout->addWidget(m_refreshButton, 0, 3);

	// Audio level indicator
	audioLayout->addWidget(new QLabel(T("Level"), this), 1, 0);
	m_audioLevelBar = new QProgressBar(this);
	m_audioLevelBar->setRange(0, 100);
	m_audioLevelBar->setValue(0);
	m_audioLevelBar->setTextVisible(false);
	m_audioLevelBar->setStyleSheet("QProgressBar {"
				       "  border: 1px solid #999;"
				       "  border-radius: 3px;"
				       "  background-color: #333;"
				       "}"
				       "QProgressBar::chunk {"
				       "  background-color: qlineargradient(x1: 0, y1: 0, x2: 1, y2: 0,"
				       "    stop: 0 #00ff00, stop: 0.8 #ffff00, stop: 1 #ff0000);"
				       "  border-radius: 2px;"
				       "}");
	audioLayout->addWidget(m_audioLevelBar, 1, 1, 1, 3);

	audioLayout->addWidget(new QLabel(T("Gain"), this), 2, 0);
	m_gainSpinBox = new QDoubleSpinBox(this);
	m_gainSpinBox->setRange(1.0, 8.0);
	m_gainSpinBox->setSingleStep(0.5);
	m_gainSpinBox->setDecimals(1);
	m_gainSpinBox->setSuffix("x");
	m_gainSpinBox->setValue(1.0);
	m_gainSpinBox->setToolTip(T("GainTooltip"));
	audioLayout->addWidget(m_gainSpinBox, 2, 1);

	mainLayout->addWidget(audioGroup);

	// Status Group
	auto *statusGroup = new QGroupBox(T("Status"), this);
	auto *statusLayout = new QVBoxLayout(statusGroup);

	m_statusLabel = new QLabel(T("NotStreaming"), this);
	m_statusLabel->setStyleSheet("QLabel { font-weight: bold; }");
	statusLayout->addWidget(m_statusLabel);

	m_dataRateLabel = new QLabel(T("DataRate").arg(0.0, 0, 'f', 1), this);
	statusLayout->addWidget(m_dataRateLabel);

	m_muteStatusLabel = new QLabel("", this);
	m_muteStatusLabel->setStyleSheet("QLabel { color: orange; font-weight: bold; }");
	statusLayout->addWidget(m_muteStatusLabel);

	mainLayout->addWidget(statusGroup);

	// Control Buttons
	auto *buttonLayout = new QHBoxLayout();

	m_startStopButton = new QPushButton(T("StartStreaming"), this);
	// Enable if audio source is selected
	m_startStopButton->setEnabled(false);
	buttonLayout->addWidget(m_startStopButton);

	auto *closeButton = new QPushButton(T("Close"), this);
	connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
	buttonLayout->addWidget(closeButton);

	mainLayout->addLayout(buttonLayout);

	// Initial state
	populateAudioSources();
}

void SettingsDialog::connectSignals()
{
	connect(m_testButton, &QPushButton::clicked, this, &SettingsDialog::onTestConnection);
	connect(m_startStopButton, &QPushButton::clicked, this, &SettingsDialog::onStartStopToggled);
	connect(m_audioSourceCombo, &QComboBox::currentTextChanged, this, &SettingsDialog::onAudioSourceChanged);
	connect(m_urlEdit, &QLineEdit::textChanged, this, &SettingsDialog::onUrlChanged);
	connect(m_autoConnectCheckBox, &QCheckBox::toggled, this, &SettingsDialog::onAutoConnectToggled);
	connect(m_gainSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
		&SettingsDialog::onGainChanged);

	// Connect thread-safe test connection error signal
	connect(this, &SettingsDialog::testConnectionError, this, &SettingsDialog::onTestConnectionError,
		Qt::QueuedConnection);

	// Connect to AudioStreamer signals
	connect(m_streamer, &AudioStreamer::connectionStatusChanged, this, &SettingsDialog::updateConnectionStatus);
	connect(m_streamer, &AudioStreamer::streamingStatusChanged, this, &SettingsDialog::updateStreamingStatus);
	connect(m_streamer, &AudioStreamer::dataRateChanged, this, &SettingsDialog::updateDataRate);
	connect(m_streamer, &AudioStreamer::errorOccurred, this, &SettingsDialog::showError);
}

void SettingsDialog::loadSettings()
{
	// Load from OBS user config
#if LIBOBS_API_MAJOR_VER >= 31
	config_t *config = obs_frontend_get_user_config();
#else
	config_t *config = obs_frontend_get_profile_config();
#endif

	// Setting a widget below would otherwise fire its change slot, which saves every
	// field and overwrites the stored values before they are read.
	const QSignalBlocker urlBlocker(m_urlEdit);
	const QSignalBlocker sourceBlocker(m_audioSourceCombo);
	const QSignalBlocker autoConnectBlocker(m_autoConnectCheckBox);
	const QSignalBlocker gainBlocker(m_gainSpinBox);

	const char *url = config_get_string(config, "AudioStreamer", "WebSocketUrl");
	if (url && strlen(url) > 0) {
		m_urlEdit->setText(url);
		m_streamer->SetWebSocketUrl(url);
	} else {
		m_urlEdit->setText(QString::fromStdString(m_streamer->GetWebSocketUrl()));
	}

	const char *source = config_get_string(config, "AudioStreamer", "AudioSource");
	if (source && strlen(source) > 0) {
		int index = m_audioSourceCombo->findText(source);
		if (index >= 0) {
			m_audioSourceCombo->setCurrentIndex(index);
			m_streamer->SetAudioSource(source);
			// Enable start button if source is valid
			m_startStopButton->setEnabled(true);
		}
	}

	bool autoConnect = config_get_bool(config, "AudioStreamer", "AutoConnect");
	m_autoConnectCheckBox->setChecked(autoConnect);
	m_streamer->SetAutoConnectEnabled(autoConnect);

	m_gainSpinBox->setValue(static_cast<double>(m_streamer->GetTranscriptionGain()));
}

bool SettingsDialog::saveSettings()
{
	// Silently fail if UI elements don't exist yet
	if (!m_urlEdit || !m_audioSourceCombo || !m_autoConnectCheckBox || !m_gainSpinBox) {
		return false;
	}

	// Get OBS config
#if LIBOBS_API_MAJOR_VER >= 31
	config_t *config = obs_frontend_get_user_config();
#else
	config_t *config = obs_frontend_get_profile_config();
#endif

	if (!config) {
		// OBS not available (shutting down?), silently fail
		blog(LOG_WARNING, "[Audio to WebSocket] Cannot save settings - OBS config not available");
		return false;
	}

	// Save settings
	std::string urlStdString = m_urlEdit->text().trimmed().toStdString();
	std::string audioSourceStdString = m_audioSourceCombo->currentText().toStdString();
	config_set_string(config, "AudioStreamer", "WebSocketUrl", urlStdString.c_str());
	config_set_string(config, "AudioStreamer", "AudioSource", audioSourceStdString.c_str());
	config_set_bool(config, "AudioStreamer", "AutoConnect", m_autoConnectCheckBox->isChecked());
	config_set_double(config, "AudioStreamer", "TranscriptionGain", m_gainSpinBox->value());

	config_save(config);
	return true;
}

void SettingsDialog::onStartStopToggled()
{
	if (m_streamer->IsStreaming()) {
		m_streamer->Stop();
	} else {
		m_streamer->Start();
	}
}

void SettingsDialog::onTestConnection()
{
	QString url = m_urlEdit->text().trimmed();
	if (url.isEmpty()) {
		QMessageBox::warning(this, T("NoUrl"), T("NoUrlMessage"));
		return;
	}

	// Validate URL format
	if (!url.startsWith("ws://")) {
		QMessageBox::warning(this, T("InvalidUrl"), T("InvalidUrlScheme"));
		return;
	}

	// Basic URL validation - check for host and path
	QUrl qurl(url);
	if (!qurl.isValid() || qurl.host().isEmpty()) {
		QMessageBox::warning(this, T("InvalidUrl"), T("InvalidUrlMessage"));
		return;
	}

	// Test WebSocket connection without affecting current state
	m_testButton->setEnabled(false);
	m_testButton->setText(T("Testing"));

	// Store original status
	QString originalStatus = m_statusLabel->text();
	QString originalStyle = m_statusLabel->styleSheet();

	// Update status to show testing
	m_statusLabel->setText(T("TestingConnection"));
	m_statusLabel->setStyleSheet("QLabel { font-weight: bold; color: blue; }");

	// Create a temporary WebSocket client for testing
	auto testClient = std::make_shared<WebSocketPPClient>();

	// Capture error messages using thread-safe signal
	QString errorMsg;
	testClient->SetOnError(
		[this](const std::string &error) { emit testConnectionError(QString::fromStdString(error)); });

	testClient->Connect(url.toStdString());

	QTimer::singleShot(2000, this, // 2 second timeout
			   [this, testClient, originalStatus, originalStyle, errorMsg]() {
				   m_testButton->setEnabled(true);
				   m_testButton->setText(T("TestConnection"));

				   if (testClient->IsConnected()) {
					   testClient->Disconnect();
					   m_statusLabel->setText(T("TestSuccessful"));
					   m_statusLabel->setStyleSheet("QLabel { font-weight: bold; color: green; }");
					   QMessageBox::information(this, T("ConnectionTest"),
								    T("ConnectionTestSuccessful"));
				   } else {
					   m_statusLabel->setText(T("TestFailed"));
					   m_statusLabel->setStyleSheet("QLabel { font-weight: bold; color: red; }");

					   QString message = T("ConnectionTestFailed");
					   if (!errorMsg.isEmpty()) {
						   message += " " + errorMsg;
					   }
					   QMessageBox::warning(this, T("ConnectionTest"), message);
				   }

				   // Restore original status after a delay
				   QTimer::singleShot(2000, this, [this, originalStatus, originalStyle]() {
					   m_statusLabel->setText(originalStatus);
					   m_statusLabel->setStyleSheet(originalStyle);
				   });
			   });
}

void SettingsDialog::onAudioSourceChanged(const QString &source)
{
	m_streamer->SetAudioSource(source.toStdString());
	// Enable/disable start button based on whether source is selected
	if (!m_streamer->IsStreaming()) {
		m_startStopButton->setEnabled(!source.isEmpty());
	}
	// Save settings immediately
	saveSettings();
}

void SettingsDialog::onUrlChanged(const QString &url)
{
	m_streamer->SetWebSocketUrl(url.toStdString());
	// Save settings immediately
	saveSettings();
}

void SettingsDialog::onAutoConnectToggled(bool enabled)
{
	m_streamer->SetAutoConnectEnabled(enabled);
	// Update status to reflect auto-connect state
	updateConnectionStatus(m_streamer->IsConnected());
	// Save settings immediately
	saveSettings();
}

void SettingsDialog::onGainChanged(double gain)
{
	m_streamer->SetTranscriptionGain(static_cast<float>(gain));
	saveSettings();
}

void SettingsDialog::updateConnectionStatus(bool connected)
{
	// Update status based on both connection and streaming state
	if (m_streamer->IsStreaming()) {
		if (connected) {
			if (m_streamer->IsAutoConnectEnabled()) {
				m_statusLabel->setText(T("AutoConnectActive"));
				m_statusLabel->setStyleSheet("QLabel { font-weight: bold; color: green; }");
			} else {
				m_statusLabel->setText(T("StreamingConnected"));
				m_statusLabel->setStyleSheet("QLabel { font-weight: bold; color: green; }");
			}
		} else {
			// Check if we're in reconnection phase
			auto wsClient = m_streamer->GetWebSocketClient();
			if (wsClient && wsClient->IsReconnecting()) {
				int attempts = wsClient->GetReconnectAttempts();
				m_statusLabel->setText(T("StreamingReconnecting").arg(attempts));
				m_statusLabel->setStyleSheet("QLabel { font-weight: bold; color: orange; }");
			} else {
				m_statusLabel->setText(T("StreamingDisconnected"));
				m_statusLabel->setStyleSheet("QLabel { font-weight: bold; color: red; }");
			}
		}
	} else {
		if (m_streamer->IsAutoConnectEnabled()) {
			m_statusLabel->setText(T("AutoConnectWaiting"));
			m_statusLabel->setStyleSheet("QLabel { font-weight: bold; color: blue; }");
		} else {
			m_statusLabel->setText(T("NotStreaming"));
			m_statusLabel->setStyleSheet("QLabel { font-weight: bold; }");
		}
	}
}

void SettingsDialog::updateStreamingStatus(bool streaming)
{
	if (streaming) {
		m_startStopButton->setText(T("StopStreaming"));
		// Disable manual start/stop when auto-connect is enabled
		if (m_streamer->IsAutoConnectEnabled()) {
			m_startStopButton->setEnabled(false);
			m_startStopButton->setToolTip(T("AutoConnectControlling"));
		} else {
			m_startStopButton->setEnabled(true);
			m_startStopButton->setToolTip("");
		}
		// Disable changing settings while streaming
		m_audioSourceCombo->setEnabled(false);
		m_refreshButton->setEnabled(false);
		m_urlEdit->setEnabled(false);
		m_testButton->setEnabled(false);
	} else {
		m_startStopButton->setText(T("StartStreaming"));
		m_startStopButton->setToolTip("");
		// Re-enable controls when not streaming
		m_audioSourceCombo->setEnabled(true);
		m_refreshButton->setEnabled(true);
		m_urlEdit->setEnabled(true);
		m_testButton->setEnabled(true);
		// Start button enabled when audio source is selected
		m_startStopButton->setEnabled(!m_audioSourceCombo->currentText().isEmpty());
	}

	// Update the status label to reflect streaming state
	updateConnectionStatus(m_streamer->IsConnected());
}

void SettingsDialog::updateDataRate(double kbps)
{
	m_dataRateLabel->setText(T("DataRate").arg(kbps, 0, 'f', 1));
}

void SettingsDialog::showError(const QString &error)
{
	// Rate limit error dialogs to prevent spam
	auto now = std::chrono::steady_clock::now();
	auto timeSinceLastError = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastErrorTime);

	// Skip if same error within 5 seconds
	if (error == m_lastErrorMessage && timeSinceLastError.count() < 5) {
		return;
	}

	m_lastErrorTime = now;
	m_lastErrorMessage = error;

	// Handle "max reconnection attempts" specially - this should stop streaming
	if (error.contains("Max reconnection attempts exceeded")) {
		m_statusLabel->setText(T("NotStreamingConnectionFailed"));
		m_statusLabel->setStyleSheet("QLabel { font-weight: bold; color: red; }");
		// Show one final dialog to inform the user
		QMessageBox::warning(this, T("ConnectionLost"), T("ConnectionLostMessage"));
		return;
	}

	// Only show dialog boxes for critical errors, not connection failures during streaming
	// Connection status is already shown in the UI status label
	if (!m_streamer->IsStreaming()) {
		// Show dialog only when not actively streaming (e.g., during initial setup)
		QMessageBox::warning(this, T("AudioStreamerError"), error);
	} else {
		// During streaming, just update the status label instead of showing a dialog
		m_statusLabel->setText(T("StreamingConnectionError"));
		m_statusLabel->setStyleSheet("QLabel { font-weight: bold; color: red; }");
		std::string errorStdString = error.toStdString();
		blog(LOG_WARNING, "[Audio to WebSocket] Error during streaming: %s", errorStdString.c_str());
	}
}

void SettingsDialog::onTestConnectionError(const QString &error)
{
	// This slot runs on the main thread, so it's safe to access UI elements
	blog(LOG_WARNING, "[Audio to WebSocket] Test connection error received: %s", error.toStdString().c_str());

	// The error will be handled in the test timeout callback
	// This just ensures the error message is logged safely
}

void SettingsDialog::updateStatus()
{
	// Update audio level and mute status
	if (!m_audioSourceCombo->currentText().isEmpty()) {
		std::string sourceNameStdString = m_audioSourceCombo->currentText().toStdString();
		obs_source_t *source = obs_get_source_by_name(sourceNameStdString.c_str());
		if (source) {
			// Update volume meter attachment
			cleanupVolumeMeter();

			// Create and attach new volmeter
			m_volmeter = obs_volmeter_create(OBS_FADER_LOG);
			obs_volmeter_add_callback(m_volmeter, volumeCallback, this);
			obs_volmeter_attach_source(m_volmeter, source);

			// Show current peak level
			float db = m_currentPeak;
			if (db < -60.0f)
				db = -60.0f;
			if (db > 0.0f)
				db = 0.0f;

			// Convert to 0-100 range
			int level = static_cast<int>((db + 60.0f) / 60.0f * 100.0f);
			m_audioLevelBar->setValue(level);

			// Check mute status
			if (m_streamer->IsStreaming()) {
				bool muted = obs_source_muted(source);
				if (muted) {
					m_muteStatusLabel->setText(T("SourceMuted"));
					m_muteStatusLabel->show();
				} else {
					m_muteStatusLabel->hide();
				}
			} else {
				m_muteStatusLabel->hide();
			}

			obs_source_release(source);
		} else {
			m_audioLevelBar->setValue(0);
			m_muteStatusLabel->hide();

			// Clean up volmeter if source not found
			cleanupVolumeMeter();
		}
	} else {
		m_audioLevelBar->setValue(0);
		m_muteStatusLabel->hide();

		// Clean up volmeter if no source selected
		cleanupVolumeMeter();
	}
}

void SettingsDialog::populateAudioSources()
{
	// Save current selection
	QString currentSelection = m_audioSourceCombo->currentText();

	m_audioSourceCombo->clear();

	// Collect sources in vectors for sorting
	struct AudioSourceInfo {
		QString name;
		QString id;
		int priority; // Lower number = higher priority
	};
	std::vector<AudioSourceInfo> sources;

	// Enumerate all audio sources
	auto enumCallback = [](void *param, obs_source_t *source) -> bool {
		auto *sources = static_cast<std::vector<AudioSourceInfo> *>(param);

		uint32_t flags = obs_source_get_output_flags(source);
		if (flags & OBS_SOURCE_AUDIO) {
			const char *id = obs_source_get_id(source);
			const char *name = obs_source_get_name(source);

			if (name && id) {
				AudioSourceInfo info;
				info.name = QString(name);
				info.id = QString(id);

				// Prioritize microphones and audio inputs
				if (strstr(id, "input_capture") || strstr(id, "mic")) {
					info.priority = 1;
				} else if (strstr(id, "output_capture")) {
					info.priority = 2;
				} else if (strcmp(id, "browser_source") == 0) {
					info.priority = 4;
				} else {
					info.priority = 3;
				}

				sources->push_back(info);
			}
		}

		return true;
	};

	obs_enum_sources(enumCallback, &sources);

	// Sort by priority, then by name
	std::sort(sources.begin(), sources.end(), [](const AudioSourceInfo &a, const AudioSourceInfo &b) {
		if (a.priority != b.priority)
			return a.priority < b.priority;
		return a.name < b.name;
	});

	// Add sorted sources
	for (const auto &source : sources) {
		m_audioSourceCombo->addItem(source.name, source.name);
	}

	// Restore previous selection if it still exists
	if (!currentSelection.isEmpty()) {
		int index = m_audioSourceCombo->findText(currentSelection);
		if (index >= 0) {
			m_audioSourceCombo->setCurrentIndex(index);
		} else {
			// Source no longer exists, show warning
			m_statusLabel->setText(T("PreviousSourceNotFound"));
			m_statusLabel->setStyleSheet("QLabel { font-weight: bold; color: orange; }");
			QTimer::singleShot(3000, this, [this]() { updateConnectionStatus(m_streamer->IsConnected()); });
		}
	}
}

void SettingsDialog::selectDefaultMicrophoneSource()
{
	// Look for any microphone source
	for (int i = 0; i < m_audioSourceCombo->count(); ++i) {
		QString itemText = m_audioSourceCombo->itemText(i);
		if (itemText.contains("mic", Qt::CaseInsensitive) || itemText.contains("input", Qt::CaseInsensitive)) {
			m_audioSourceCombo->setCurrentIndex(i);
			return;
		}
	}
}

void SettingsDialog::cleanupVolumeMeter()
{
	if (!m_volmeter) {
		return;
	}

	try {
		// Safely remove callback - this can fail if OBS is shutting down
		obs_volmeter_remove_callback(m_volmeter, volumeCallback, this);
	} catch (...) {
		// Log error but continue with cleanup
		blog(LOG_WARNING, "[Audio to WebSocket] Exception during volume meter callback removal");
	}

	try {
		// Destroy the volume meter - this should not fail but be defensive
		obs_volmeter_destroy(m_volmeter);
	} catch (...) {
		// Log error but continue
		blog(LOG_ERROR, "[Audio to WebSocket] Exception during volume meter destruction");
	}

	// Always nullify the pointer regardless of cleanup success
	m_volmeter = nullptr;
}

void SettingsDialog::volumeCallback(void *data, const float magnitude[MAX_AUDIO_CHANNELS],
				    const float peak[MAX_AUDIO_CHANNELS], const float inputPeak[MAX_AUDIO_CHANNELS])
{
	UNUSED_PARAMETER(magnitude);
	UNUSED_PARAMETER(inputPeak);

	auto *dialog = static_cast<SettingsDialog *>(data);

	// Use the highest peak from all channels
	float maxPeak = -60.0f;
	for (int i = 0; i < MAX_AUDIO_CHANNELS; i++) {
		if (peak[i] > maxPeak) {
			maxPeak = peak[i];
		}
	}

	dialog->m_currentPeak = maxPeak;
}

} // namespace obs_audio_to_websocket
