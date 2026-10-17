#pragma once

#include "relay_runtime.h"
#include "ui_relay_station.h"

#include <QtWidgets/QMainWindow>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>

#include "qcustomplot.h"
#include <opencv2/opencv.hpp>

#include <atomic>
#include <optional>

// Dedicated network-latency worker. It owns all port-8890 sockets in its thread.
class PingWorker : public QObject {
    Q_OBJECT

public:
    explicit PingWorker(QObject* parent = nullptr) : QObject(parent) {}

    QElapsedTimer* timer = nullptr;
    int currentPayloadIndex = 0; // 0: 32 B, 1: 50 KiB, 2: 3 MiB

signals:
    void resultReady(double latencyMs);
    void logToUi(const QString& message);
    void pongReceived();

public slots:
    void setup();
    void stopAll();
    void sendPing(int payloadSize, bool isUdp);

private slots:
    void handleTcpNewConnection();
    void handleTcpReadyRead();
    void handleUdpReadyRead();
    void processData(const QByteArray& data, qint64 receiveSteadyNs);

private:
    QTcpServer* tcpServer = nullptr;
    QTcpSocket* tcpClient = nullptr;
    QUdpSocket* udpSocket = nullptr;
    QHostAddress udpTargetIp;
    quint16 udpTargetPort = 0;
    QByteArray txBuffer;
    int lastPayloadSize = -1;
};

// Captures and encodes camera frames away from the UI thread. The cross-thread
// queue contains at most one encoded frame, so a slow UDP sender cannot build an
// unbounded backlog. The production UDP-8886 payload stays byte-compatible.
class VideoStreamThread : public QThread {
    Q_OBJECT

public:
    explicit VideoStreamThread(QObject* parent = nullptr) : QThread(parent) {}

    void startStreaming()
    {
        if (!m_running.exchange(true)) {
            start();
        }
    }

    void stopStreaming()
    {
        m_running = false;
        wait();
        m_frames.clear();
    }

    std::optional<relay::EncodedCameraFrame> takeLatestFrame() { return m_frames.take(); }
    quint64 captureFailures() const { return m_captureFailures.load(); }
    quint64 encodeFailures() const { return m_encodeFailures.load(); }
    quint64 droppedFrames() const { return m_droppedFrames.load() + m_frames.replacementCount(); }

signals:
    void frameAvailable();
    void cameraStatusChanged(bool isOpen);

protected:
    void run() override
    {
        cv::VideoCapture capture(0, cv::CAP_DSHOW);
        if (!capture.isOpened()) {
            ++m_captureFailures;
            m_running = false;
            emit cameraStatusChanged(false);
            return;
        }
        emit cameraStatusChanged(true);
        capture.set(cv::CAP_PROP_FRAME_WIDTH, 640);
        capture.set(cv::CAP_PROP_FRAME_HEIGHT, 480);

        cv::Mat frame;
        std::vector<uchar> buffer;
        const std::vector<int> encodeParameters = {cv::IMWRITE_JPEG_QUALITY, 60};
        while (m_running) {
            const qint64 captureNs = relay::steadyNowNs();
            capture >> frame;
            if (frame.empty()) {
                ++m_captureFailures;
                QThread::msleep(33);
                continue;
            }

            buffer.clear();
            bool encoded = false;
            try {
                encoded = cv::imencode(".jpg", frame, buffer, encodeParameters);
            } catch (const cv::Exception&) {
                encoded = false;
            }
            const qint64 encodedNs = relay::steadyNowNs();
            if (!encoded || buffer.empty()) {
                ++m_encodeFailures;
            } else if (buffer.size() >= 65500) {
                // Keep UDP-8886 compatible: oversized legacy datagrams are not sent.
                ++m_droppedFrames;
            } else {
                relay::EncodedCameraFrame encodedFrame;
                encodedFrame.bytes = QByteArray(reinterpret_cast<const char*>(buffer.data()),
                                                static_cast<qsizetype>(buffer.size()));
                encodedFrame.metadata.frameSeq = ++m_frameSeq;
                encodedFrame.metadata.captureSteadyNs = captureNs;
                encodedFrame.metadata.encodedSteadyNs = encodedNs;
                encodedFrame.metadata.width = frame.cols;
                encodedFrame.metadata.height = frame.rows;
                encodedFrame.metadata.encoding = QStringLiteral("jpeg");
                encodedFrame.metadata.byteCount = encodedFrame.bytes.size();
                m_frames.offer(encodedFrame);
                emit frameAvailable();
            }
            QThread::msleep(33);
        }
        capture.release();
        m_running = false;
    }

private:
    std::atomic<bool> m_running{false};
    std::atomic<quint64> m_frameSeq{0};
    std::atomic<quint64> m_captureFailures{0};
    std::atomic<quint64> m_encodeFailures{0};
    std::atomic<quint64> m_droppedFrames{0};
    relay::LatestFrameQueue m_frames;
};

class RelayStation : public QMainWindow {
    Q_OBJECT

public:
    explicit RelayStation(QWidget* parent = nullptr);
    ~RelayStation() override;

private slots:
    void on_btn_start_clicked();
    void on_btn_stop_clicked();
    void onRobotStateAvailable();
    void onUdpCommandReadyRead();
    void onUdpTwinReadyRead();
    void onUdpForceReadyRead();
    void updateLatencyFromWorker(double latencyMs);

private:
    void initProcessLatencyPlot();
    void updateProcessLatencyUI(int channelIndex, double latencyMs);
    void updateNetworkLatencyUI(double netLatency);
    void updateRobotIndicator();
    void syncStreamDiagnostics();
    void ensureVideoThread();
    void stopCompatibilityStreams();

    Ui::RelayStationClass ui;
    relay::RelayRuntime* m_runtime = nullptr;

    QThread* m_latencyThread = nullptr;
    PingWorker* m_worker = nullptr;
    bool m_isWaitingPONG = false;
    QElapsedTimer m_netTestTimer;
    QTimer* m_netTimer = nullptr;

    int m_packetCountControl = 0;
    int m_packetCountTwin = 0;
    int m_packetCountVideo = 0;
    int m_packetCountFeedback = 0;
    int m_packetCountNetwork = 0;
    QList<double> m_latencyHistory[4];
    QList<double> m_networkLatencyHistory;

    VideoStreamThread* m_videoThread = nullptr;
    QHostAddress m_videoTargetIp;
    quint16 m_videoTargetPort = 0;

    QUdpSocket* m_udpCommandSocket = nullptr;
    QTimer* m_videoHeartbeatTimer = nullptr;
    QUdpSocket* m_udpTwinSocket = nullptr;
    QTimer* m_twinHeartbeatTimer = nullptr;
    QHostAddress m_twinTargetIp;
    quint16 m_twinTargetPort = 0;
    QUdpSocket* m_udpForceSocket = nullptr;
    QTimer* m_forceHeartbeatTimer = nullptr;
    QHostAddress m_forceTargetIp;
    quint16 m_forceTargetPort = 0;
    relay::StreamDiagnostics m_streamDiagnostics;
    quint64 m_cameraDispatchDrops = 0;
};
