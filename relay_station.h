#pragma once

#include "relay_runtime.h"
#include "ui_relay_station.h"

#include <QtWidgets/QMainWindow>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QMutex>
#include <QMutexLocker>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>
#include <QWaitCondition>

#include "qcustomplot.h"
#include <opencv2/opencv.hpp>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

// Performs the CPU-heavy OpenCV analysis and JPEG encoding independently from
// camera capture. Both its input and output are latest-frame queues so temporary
// CPU/network stalls increase the drop count instead of accumulating latency.
class OpenCvProcessingThread : public QThread {
    Q_OBJECT

public:
    explicit OpenCvProcessingThread(QObject* parent = nullptr) : QThread(parent) {}
    ~OpenCvProcessingThread() override { stopProcessing(); }

    void startProcessing()
    {
        if (m_running.exchange(true)) return;
        {
            QMutexLocker locker(&m_inputMutex);
            m_pendingFrame.reset();
        }
        m_frames.clear();
        start();
    }

    void stopProcessing()
    {
        {
            QMutexLocker locker(&m_inputMutex);
            m_running = false;
            m_pendingFrame.reset();
            m_inputReady.wakeAll();
        }
        if (isRunning()) wait();
        m_frames.clear();
    }

    void offerFrame(const cv::Mat& frame, quint64 frameSeq, qint64 captureSteadyNs)
    {
        if (!m_running || frame.empty()) return;
        RawCameraFrame incoming;
        incoming.image = frame.clone();
        incoming.frameSeq = frameSeq;
        incoming.captureSteadyNs = captureSteadyNs;
        {
            QMutexLocker locker(&m_inputMutex);
            if (!m_running) return;
            if (m_pendingFrame.has_value()) ++m_inputDrops;
            m_pendingFrame = std::move(incoming);
        }
        m_inputReady.wakeOne();
    }

    std::optional<relay::EncodedCameraFrame> takeLatestFrame() { return m_frames.take(); }
    quint64 processingFailures() const { return m_processingFailures.load(); }
    quint64 encodeFailures() const { return m_encodeFailures.load(); }
    quint64 droppedFrames() const
    {
        return m_inputDrops.load() + m_oversizedFrames.load() + m_frames.replacementCount();
    }

signals:
    void frameAvailable();

protected:
    void run() override
    {
        qint64 previousProcessedNs = 0;
        double smoothedFps = 0.0;
        while (m_running) {
            RawCameraFrame input;
            {
                QMutexLocker locker(&m_inputMutex);
                while (m_running && !m_pendingFrame.has_value()) {
                    m_inputReady.wait(&m_inputMutex);
                }
                if (!m_running) break;
                input = std::move(*m_pendingFrame);
                m_pendingFrame.reset();
            }

            try {
                cv::Mat annotated = input.image.clone();
                cv::Mat gray;
                cv::cvtColor(input.image, gray, cv::COLOR_BGR2GRAY);

                cv::Scalar meanValue;
                cv::Scalar deviation;
                cv::meanStdDev(gray, meanValue, deviation);
                const double brightness = meanValue[0];
                const double contrast = deviation[0];

                cv::Mat blurred;
                cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 1.2);

                std::vector<cv::Point2f> corners;
                cv::goodFeaturesToTrack(blurred, corners, 80, 0.015, 10.0,
                                        cv::noArray(), 3, false, 0.04);

                const double lowerCanny = std::max(30.0, brightness * 0.55);
                const double upperCanny = std::min(220.0, std::max(80.0, brightness * 1.45));
                cv::Mat edges;
                cv::Canny(blurred, edges, lowerCanny, upperCanny, 3, true);

                std::vector<cv::Vec4i> lines;
                cv::HoughLinesP(edges, lines, 1.0, CV_PI / 180.0, 45, 40.0, 12.0);
                std::sort(lines.begin(), lines.end(), [](const cv::Vec4i& lhs, const cv::Vec4i& rhs) {
                    return std::hypot(static_cast<double>(lhs[2] - lhs[0]),
                                      static_cast<double>(lhs[3] - lhs[1])) >
                           std::hypot(static_cast<double>(rhs[2] - rhs[0]),
                                      static_cast<double>(rhs[3] - rhs[1]));
                });
                if (lines.size() > 12) lines.resize(12);

                double directionX = 0.0;
                double directionY = 0.0;
                for (const cv::Vec4i& line : lines) {
                    const cv::Point start(line[0], line[1]);
                    const cv::Point end(line[2], line[3]);
                    const double length = std::hypot(static_cast<double>(end.x - start.x),
                                                     static_cast<double>(end.y - start.y));
                    const double angle = std::atan2(static_cast<double>(end.y - start.y),
                                                    static_cast<double>(end.x - start.x));
                    directionX += length * std::cos(2.0 * angle);
                    directionY += length * std::sin(2.0 * angle);
                    cv::line(annotated, start, end, cv::Scalar(40, 230, 40), 2, cv::LINE_AA);
                    cv::circle(annotated, start, 3, cv::Scalar(255, 220, 0), cv::FILLED, cv::LINE_AA);
                    cv::circle(annotated, end, 3, cv::Scalar(255, 220, 0), cv::FILLED, cv::LINE_AA);
                }

                for (const cv::Point2f& corner : corners) {
                    cv::circle(annotated, corner, 4, cv::Scalar(0, 80, 255), 1, cv::LINE_AA);
                }

                double dominantDegrees = 0.0;
                if (!lines.empty()) {
                    dominantDegrees = 0.5 * std::atan2(directionY, directionX) * 180.0 / CV_PI;
                    if (dominantDegrees < 0.0) dominantDegrees += 180.0;
                }

                cv::Mat laplacian;
                cv::Laplacian(gray, laplacian, CV_64F);
                cv::Scalar laplacianMean;
                cv::Scalar laplacianDeviation;
                cv::meanStdDev(laplacian, laplacianMean, laplacianDeviation);
                const double sharpness = laplacianDeviation[0] * laplacianDeviation[0];
                const double edgePercent = 100.0 * static_cast<double>(cv::countNonZero(edges)) /
                                           static_cast<double>(edges.total());

                const qint64 processedNs = relay::steadyNowNs();
                if (previousProcessedNs > 0 && processedNs > previousProcessedNs) {
                    const double instantFps = 1.0e9 / static_cast<double>(processedNs - previousProcessedNs);
                    smoothedFps = smoothedFps <= 0.0 ? instantFps : 0.85 * smoothedFps + 0.15 * instantFps;
                }
                previousProcessedNs = processedNs;

                const int panelWidth = std::min(annotated.cols, 315);
                const int panelHeight = std::min(annotated.rows, 170);
                cv::Mat overlay = annotated.clone();
                cv::rectangle(overlay, cv::Rect(0, 0, panelWidth, panelHeight),
                              cv::Scalar(10, 10, 10), cv::FILLED);
                cv::addWeighted(overlay, 0.68, annotated, 0.32, 0.0, annotated);

                const auto putMetric = [&annotated](const std::string& text, int row,
                                                    const cv::Scalar& color = cv::Scalar(245, 245, 245)) {
                    cv::putText(annotated, text, cv::Point(10, 23 + row * 22),
                                cv::FONT_HERSHEY_SIMPLEX, 0.52, color, 1, cv::LINE_AA);
                };
                putMetric(cv::format("Frame: %llu   FPS: %.1f",
                                     static_cast<unsigned long long>(input.frameSeq), smoothedFps), 0,
                          cv::Scalar(80, 220, 255));
                putMetric(cv::format("Corners: %d   Main lines: %d",
                                     static_cast<int>(corners.size()), static_cast<int>(lines.size())), 1);
                putMetric(cv::format("Dominant direction: %.1f deg", dominantDegrees), 2);
                putMetric(cv::format("Brightness: %.1f   Contrast: %.1f", brightness, contrast), 3);
                putMetric(cv::format("Sharpness: %.0f   Edges: %.1f%%", sharpness, edgePercent), 4);
                putMetric(cv::format("Resolution: %d x %d", annotated.cols, annotated.rows), 5);

                const cv::Point center(annotated.cols / 2, annotated.rows / 2);
                cv::drawMarker(annotated, center, cv::Scalar(0, 255, 255),
                               cv::MARKER_CROSS, 20, 1, cv::LINE_AA);

                std::vector<uchar> buffer;
                bool encoded = false;
                // Preserve the legacy one-JPEG-per-datagram protocol. If overlays
                // make a frame too large, lower JPEG quality before dropping it.
                for (const int quality : {60, 50, 40}) {
                    buffer.clear();
                    encoded = cv::imencode(".jpg", annotated, buffer,
                                           {cv::IMWRITE_JPEG_QUALITY, quality});
                    if (!encoded || buffer.empty() || buffer.size() < 65500) break;
                }
                const qint64 encodedNs = relay::steadyNowNs();
                if (!encoded || buffer.empty()) {
                    ++m_encodeFailures;
                    continue;
                }
                if (buffer.size() >= 65500) {
                    ++m_oversizedFrames;
                    continue;
                }

                relay::EncodedCameraFrame output;
                output.bytes = QByteArray(reinterpret_cast<const char*>(buffer.data()),
                                          static_cast<qsizetype>(buffer.size()));
                output.metadata.frameSeq = input.frameSeq;
                output.metadata.captureSteadyNs = input.captureSteadyNs;
                output.metadata.encodedSteadyNs = encodedNs;
                output.metadata.width = annotated.cols;
                output.metadata.height = annotated.rows;
                output.metadata.encoding = QStringLiteral("jpeg");
                output.metadata.byteCount = output.bytes.size();
                m_frames.offer(output);
                emit frameAvailable();
            } catch (const cv::Exception&) {
                ++m_processingFailures;
            }
        }
    }

private:
    struct RawCameraFrame {
        cv::Mat image;
        quint64 frameSeq = 0;
        qint64 captureSteadyNs = 0;
    };

    std::atomic<bool> m_running{false};
    QMutex m_inputMutex;
    QWaitCondition m_inputReady;
    std::optional<RawCameraFrame> m_pendingFrame;
    relay::LatestFrameQueue m_frames;
    std::atomic<quint64> m_inputDrops{0};
    std::atomic<quint64> m_oversizedFrames{0};
    std::atomic<quint64> m_processingFailures{0};
    std::atomic<quint64> m_encodeFailures{0};
};

// Captures camera frames away from both the UI and OpenCV processing threads.
class VideoStreamThread : public QThread {
    Q_OBJECT

public:
    explicit VideoStreamThread(QObject* parent = nullptr) : QThread(parent)
    {
        connect(&m_processor, &OpenCvProcessingThread::frameAvailable,
                this, &VideoStreamThread::frameAvailable);
    }
    ~VideoStreamThread() override { stopStreaming(); }

    void startStreaming()
    {
        if (!m_running.exchange(true)) {
            m_processor.startProcessing();
            start();
        }
    }

    void stopStreaming()
    {
        m_running = false;
        if (isRunning()) wait();
        m_processor.stopProcessing();
    }

    std::optional<relay::EncodedCameraFrame> takeLatestFrame() { return m_processor.takeLatestFrame(); }
    quint64 captureFailures() const { return m_captureFailures.load(); }
    quint64 encodeFailures() const
    {
        return m_processor.processingFailures() + m_processor.encodeFailures();
    }
    quint64 droppedFrames() const { return m_processor.droppedFrames(); }

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
        while (m_running) {
            const qint64 captureNs = relay::steadyNowNs();
            capture >> frame;
            if (frame.empty()) {
                ++m_captureFailures;
                QThread::msleep(33);
                continue;
            }
            m_processor.offerFrame(frame, ++m_frameSeq, captureNs);
            QThread::msleep(33);
        }
        capture.release();
        m_running = false;
    }

private:
    std::atomic<bool> m_running{false};
    std::atomic<quint64> m_frameSeq{0};
    std::atomic<quint64> m_captureFailures{0};
    OpenCvProcessingThread m_processor;
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
