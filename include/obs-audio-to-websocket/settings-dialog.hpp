#pragma once

#include <QDialog>
#include <QTimer>
#include <memory>
#include <chrono>
#include <obs.h>

QT_BEGIN_NAMESPACE
class QLineEdit;
class QPushButton;
class QComboBox;
class QLabel;
class QProgressBar;
class QCheckBox;
class QDoubleSpinBox;
QT_END_NAMESPACE

namespace obs_audio_to_websocket {

class AudioStreamer;

class SettingsDialog : public QDialog {
	Q_OBJECT

public:
	explicit SettingsDialog(QWidget *parent = nullptr);
	~SettingsDialog();

private slots:
	void onStartStopToggled();
	void onTestConnection();
	void onAudioSourceChanged(const QString &source);
	void onUrlChanged(const QString &url);
	void onAutoConnectToggled(bool enabled);
	void onGainChanged(double gain);

	void updateConnectionStatus(bool connected);
	void updateStreamingStatus(bool streaming);
	void updateDataRate(double kbps);
	void showError(const QString &error);
	void onTestConnectionError(const QString &error);

	void updateStatus();
	void populateAudioSources();

signals:
	void testConnectionError(const QString &error);

private:
	void setupUi();
	void connectSignals();
	void loadSettings();
	bool saveSettings();
	void selectDefaultMicrophoneSource();

	static void volumeCallback(void *data, const float magnitude[MAX_AUDIO_CHANNELS],
				   const float peak[MAX_AUDIO_CHANNELS], const float inputPeak[MAX_AUDIO_CHANNELS]);

	void cleanupVolumeMeter();

	// UI Elements
	QLineEdit *m_urlEdit;
	QPushButton *m_testButton;
	QCheckBox *m_autoConnectCheckBox;
	QComboBox *m_audioSourceCombo;
	QPushButton *m_refreshButton;
	QDoubleSpinBox *m_gainSpinBox;
	QPushButton *m_startStopButton;
	QProgressBar *m_audioLevelBar;
	QLabel *m_statusLabel;
	QLabel *m_dataRateLabel;
	QLabel *m_muteStatusLabel;

	// Update timer
	std::unique_ptr<QTimer> m_updateTimer;

	// Reference to audio streamer
	AudioStreamer *m_streamer;

	// Volume meter
	obs_volmeter_t *m_volmeter = nullptr;
	float m_currentPeak = -60.0f;

	// Error dialog rate limiting
	std::chrono::steady_clock::time_point m_lastErrorTime;
	QString m_lastErrorMessage;
};

} // namespace obs_audio_to_websocket
