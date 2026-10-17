#include "relay_station.h"
#include <QDebug> 
#include <QNetworkProxy>
#include <algorithm>
#include <cstring>

// =========================================================
// 🚀 PingWorker 核心实现：彻底剥离 UI 干扰
// =========================================================
void PingWorker::setup() {
    tcpServer = new QTcpServer(this);
    tcpServer->listen(QHostAddress::Any, 8890);
    connect(tcpServer, &QTcpServer::newConnection, this, &PingWorker::handleTcpNewConnection);

    udpSocket = new QUdpSocket(this);
    udpSocket->bind(QHostAddress::Any, 8890, QUdpSocket::ShareAddress);
    connect(udpSocket, &QUdpSocket::readyRead, this, &PingWorker::handleUdpReadyRead);

    emit logToUi(">> [Latency Thread] Ping Sockets initialized on Port 8890.");
}

void PingWorker::handleTcpNewConnection() {
    if (tcpClient) tcpClient->deleteLater();
    tcpClient = tcpServer->nextPendingConnection();
    tcpClient->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    connect(tcpClient, &QTcpSocket::readyRead, this, &PingWorker::handleTcpReadyRead);
    emit logToUi(">> [Latency Thread] TCP Probe joined Port 8890.");
}

void PingWorker::sendPing(int payloadSize, bool isUdp) {
    if (payloadSize != lastPayloadSize) {
        txBuffer.resize(payloadSize + 64);
        memset(txBuffer.data(), 'X', payloadSize);
        lastPayloadSize = payloadSize;
    }

    qint64 t1 = timer->nsecsElapsed();
    char tail[64];
    int tailLen = sprintf_s(tail, sizeof(tail), "PING|%lld|", t1);
    memcpy(txBuffer.data() + payloadSize, tail, tailLen);

    if (isUdp) {
        if (!udpTargetIp.isNull()) {
            udpSocket->writeDatagram(txBuffer.constData(), payloadSize + tailLen, udpTargetIp, udpTargetPort);
        }
    }
    else {
        if (tcpClient && tcpClient->isOpen()) {
            tcpClient->write(txBuffer.constData(), payloadSize + tailLen);
        }
    }
}

void PingWorker::handleTcpReadyRead() {
    qint64 t2 = timer->nsecsElapsed();
    if (tcpClient) processData(tcpClient->readAll(), t2);
}

void PingWorker::handleUdpReadyRead() {
    while (udpSocket && udpSocket->hasPendingDatagrams()) {
        qint64 t2 = timer->nsecsElapsed();
        QByteArray datagram;
        datagram.resize(int(udpSocket->pendingDatagramSize()));
        QHostAddress senderIp;
        quint16 senderPort;
        udpSocket->readDatagram(datagram.data(), datagram.size(), &senderIp, &senderPort);

        if (senderIp.protocol() == QAbstractSocket::IPv6Protocol) {
            senderIp = QHostAddress(senderIp.toIPv4Address());
        }

        QString cmd = QString::fromUtf8(datagram).trimmed();
        if (cmd == "PING_HOLE_PUNCH") {
            static QHostAddress lastPunchIp;
            static quint16 lastPunchPort = 0;
            if (lastPunchIp != senderIp || lastPunchPort != senderPort) {
                emit logToUi(QString(">> [Latency Thread] UDP Probe punched: %1:%2").arg(senderIp.toString()).arg(senderPort));
                lastPunchIp = senderIp;
                lastPunchPort = senderPort;
            }
            udpTargetIp = senderIp;
            udpTargetPort = senderPort;
        }
        else {
            processData(datagram, t2);
        }
    }
}

void PingWorker::processData(const QByteArray& data, qint64 t_recv) {
    int pongIdx = data.lastIndexOf("PONG|");
    if (pongIdx != -1) {
        QString pongPart = QString::fromUtf8(data.mid(pongIdx));
        QStringList parts = pongPart.split('|');

        if (parts.size() >= 3) {
            qint64 t_sent = parts[1].toLongLong();
            if (t_sent > 0) {
                double rttMs = (t_recv - t_sent) / 1000000.0;

                emit pongReceived();

                if (currentPayloadIndex == 0) {
                    emit resultReady(rttMs / 2.0);
                }
                else {
                    emit resultReady(rttMs);
                }
            }
        }
    }
}

void PingWorker::stopAll() {
    if (tcpServer) tcpServer->close();
    if (udpSocket) udpSocket->close();
    if (tcpClient && tcpClient->isOpen()) tcpClient->disconnectFromHost();
}

// =========================================================
// RelayStation 主类实现
// =========================================================
RelayStation::RelayStation(QWidget* parent)
    : QMainWindow(parent)
{
    ui.setupUi(this);
    initProcessLatencyPlot();
    ui.group_network_latency->setChecked(false);

    QComboBox* comboProtocol = ui.group_network_latency->findChild<QComboBox*>("combo_ping_protocol");
    if (ui.combo_payload_type && comboProtocol) {
        connect(ui.combo_payload_type, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, comboProtocol](int index) {
            m_networkLatencyHistory.clear();
            if (index == 2) comboProtocol->setCurrentIndex(0);
            if (m_worker) m_worker->currentPayloadIndex = index;
            });
        connect(comboProtocol, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
            m_networkLatencyHistory.clear();
            if (index == 1 && ui.combo_payload_type->currentIndex() == 2) {
                ui.combo_payload_type->setCurrentIndex(1);
            }
            });
    }
    else {
        connect(ui.combo_payload_type, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
            m_networkLatencyHistory.clear();
            if (m_worker) m_worker->currentPayloadIndex = index;
            });
    }

    m_netTestTimer.start();
    m_netTimer = new QTimer(this);
    static qint64 lastPingTimeMs = 0;

    connect(m_netTimer, &QTimer::timeout, this, [this]() {
        if (m_isWaitingPONG && (m_netTestTimer.elapsed() - lastPingTimeMs) > 3000) {
            m_isWaitingPONG = false;
        }

        if (ui.group_network_latency->isChecked() && !m_isWaitingPONG && m_worker) {
            QComboBox* protoCombo = ui.group_network_latency->findChild<QComboBox*>("combo_ping_protocol");
            bool isUdp = protoCombo ? (protoCombo->currentIndex() == 1) : false;

            int payloadSize = 32;
            if (ui.combo_payload_type) {
                int index = ui.combo_payload_type->currentIndex();
                if (index == 1) payloadSize = 1024 * 50;
                else if (index == 2) payloadSize = 1024 * 1024 * 3;
            }

            if (isUdp && payloadSize > 60000) payloadSize = 60000;

            m_isWaitingPONG = true;
            lastPingTimeMs = m_netTestTimer.elapsed();

            QMetaObject::invokeMethod(m_worker, "sendPing", Qt::QueuedConnection,
                Q_ARG(int, payloadSize), Q_ARG(bool, isUdp));
        }
        });

    ui.plot_network_latency->setBackground(QBrush(QColor("#1B212D")));
    QPen axisPen(QColor("#E2E8F0"));
    ui.plot_network_latency->xAxis->setBasePen(axisPen);
    ui.plot_network_latency->yAxis->setBasePen(axisPen);
    ui.plot_network_latency->xAxis->setTickPen(axisPen);
    ui.plot_network_latency->yAxis->setTickPen(axisPen);
    ui.plot_network_latency->xAxis->setSubTickPen(axisPen);
    ui.plot_network_latency->yAxis->setSubTickPen(axisPen);
    ui.plot_network_latency->xAxis->setTickLabelColor(QColor("#E2E8F0"));
    ui.plot_network_latency->yAxis->setTickLabelColor(QColor("#E2E8F0"));
    ui.plot_network_latency->xAxis->setLabelColor(QColor("#E2E8F0"));
    ui.plot_network_latency->yAxis->setLabelColor(QColor("#E2E8F0"));
    ui.plot_network_latency->xAxis->grid()->setPen(QPen(QColor("#31394E"), 1, Qt::DotLine));
    ui.plot_network_latency->yAxis->grid()->setPen(QPen(QColor("#31394E"), 1, Qt::DotLine));
    ui.plot_network_latency->xAxis->grid()->setZeroLinePen(Qt::NoPen);
    ui.plot_network_latency->yAxis->grid()->setZeroLinePen(Qt::NoPen);

    ui.plot_network_latency->addGraph();
    QPen netPen(QColor("#F15BB5"));
    netPen.setWidth(2);
    ui.plot_network_latency->graph(0)->setPen(netPen);
    ui.plot_network_latency->xAxis->setRange(0, 100);
    ui.plot_network_latency->xAxis->setLabel("Network Count");
    ui.plot_network_latency->yAxis->setLabel("Latency (ms)");
    ui.plot_network_latency->legend->setVisible(false);

    QFont axisLabelFont("Segoe UI", 12, QFont::Bold);
    QFont tickLabelFont("Segoe UI", 10);
    ui.plot_network_latency->xAxis->setLabelFont(axisLabelFont);
    ui.plot_network_latency->yAxis->setLabelFont(axisLabelFont);
    ui.plot_network_latency->xAxis->setTickLabelFont(tickLabelFont);
    ui.plot_network_latency->yAxis->setTickLabelFont(tickLabelFont);

    // ==========================================================
    // 🚀 核心机制 1：解除强制互斥，实现点击暂停功能
    // ==========================================================
    ui.chk_show_control->setAutoExclusive(false);
    ui.chk_show_digitaltwin->setAutoExclusive(false);
    ui.chk_show_video->setAutoExclusive(false);

    // 🚀 核心机制 2：动态图例与【历史视野秒切】机制
    auto updatePlotVisibility = [this]() {
        bool ctrl = ui.chk_show_control->isChecked();
        bool twin = ui.chk_show_digitaltwin->isChecked();
        bool vid = ui.chk_show_video->isChecked();

        // 冻结状态：全都没选中，保持画面和视野不变
        if (!ctrl && !twin && !vid) return;

        // 设置线条可见性
        ui.plot_relay_latency->graph(0)->setVisible(ctrl);
        ui.plot_relay_latency->graph(3)->setVisible(ctrl);
        ui.plot_relay_latency->graph(1)->setVisible(twin);
        ui.plot_relay_latency->graph(2)->setVisible(vid);

        // 动态踢出/加入图例
        if (ctrl) {
            ui.plot_relay_latency->graph(0)->addToLegend();
            ui.plot_relay_latency->graph(3)->addToLegend();
            ui.plot_relay_latency->graph(1)->removeFromLegend();
            ui.plot_relay_latency->graph(2)->removeFromLegend();
        }
        else if (twin) {
            ui.plot_relay_latency->graph(1)->addToLegend();
            ui.plot_relay_latency->graph(0)->removeFromLegend();
            ui.plot_relay_latency->graph(2)->removeFromLegend();
            ui.plot_relay_latency->graph(3)->removeFromLegend();
        }
        else if (vid) {
            ui.plot_relay_latency->graph(2)->addToLegend();
            ui.plot_relay_latency->graph(0)->removeFromLegend();
            ui.plot_relay_latency->graph(1)->removeFromLegend();
            ui.plot_relay_latency->graph(3)->removeFromLegend();
        }

        // 🚀【本次修复重点】：切频道时，瞬间把坐标轴推回该频道历史数据的视野！
        int refX = 0;
        double maxInWindow = 0.15;
        double sum = 0;
        int count = 0;

        if (ctrl) {
            refX = m_packetCountControl;
            for (double v : m_latencyHistory[0]) { if (v * 2.0 > maxInWindow) maxInWindow = v * 2.0; sum += v; count++; }
            for (double v : m_latencyHistory[3]) { if (v * 2.0 > maxInWindow) maxInWindow = v * 2.0; }
        }
        else if (twin) {
            refX = m_packetCountTwin;
            for (double v : m_latencyHistory[1]) { if (v * 2.0 > maxInWindow) maxInWindow = v * 2.0; sum += v; count++; }
        }
        else if (vid) {
            refX = m_packetCountVideo;
            for (double v : m_latencyHistory[2]) { if (v * 2.0 > maxInWindow) maxInWindow = v * 2.0; sum += v; count++; }
        }

        // 瞬间恢复文本框的平均值
        if (count > 0) {
            ui.avg_relay_latency->setText(QString::number(sum / count, 'f', 3));
        }
        else {
            ui.avg_relay_latency->clear();
        }

        // 瞬间恢复 Y 轴峰值
        ui.plot_relay_latency->yAxis->setRange(0, maxInWindow);

        // 瞬间拉回 X 轴视野，不再需要等新包
        if (refX > 100) ui.plot_relay_latency->xAxis->setRange(refX - 100, refX);
        else ui.plot_relay_latency->xAxis->setRange(0, 100);

        ui.plot_relay_latency->replot();
        };

    // 绑定点击事件
    connect(ui.chk_show_control, &QAbstractButton::clicked, this, [=](bool checked) {
        if (checked) {
            ui.chk_show_digitaltwin->setChecked(false);
            ui.chk_show_video->setChecked(false);
            updatePlotVisibility();
        }
        });

    connect(ui.chk_show_digitaltwin, &QAbstractButton::clicked, this, [=](bool checked) {
        if (checked) {
            ui.chk_show_control->setChecked(false);
            ui.chk_show_video->setChecked(false);
            updatePlotVisibility();
        }
        });

    connect(ui.chk_show_video, &QAbstractButton::clicked, this, [=](bool checked) {
        if (checked) {
            ui.chk_show_control->setChecked(false);
            ui.chk_show_digitaltwin->setChecked(false);
            updatePlotVisibility();
        }
        });

    // 默认开启控制流
    ui.chk_show_control->setChecked(true);

    m_runtime = new relay::RelayRuntime(this);
    connect(m_runtime, &relay::RelayRuntime::logMessage,
            ui.txt_state_log, &QPlainTextEdit::appendPlainText);
    connect(m_runtime, &relay::RelayRuntime::robotStateAvailable,
            this, &RelayStation::onRobotStateAvailable);
    connect(m_runtime, &relay::RelayRuntime::connectionStatusChanged,
            this, &RelayStation::updateRobotIndicator);
    connect(m_runtime, &relay::RelayRuntime::legacyClientChanged, this, [this](bool connected) {
        ui.LED_Touch->setStyleSheet(connected
            ? "QLabel { background-color: #00FF00; border-radius: 8px; border: 1px solid #00AA00; }"
            : "QLabel { background-color: #FF0000; border-radius: 8px; border: 1px solid #AA0000; }");
    });
    connect(m_runtime, &relay::RelayRuntime::sessionReset,
            this, &RelayStation::stopCompatibilityStreams);
    connect(m_runtime, &relay::RelayRuntime::commandRouted,
            this, [this](int targetPort, const QByteArray& command, double latencyMs) {
        updateProcessLatencyUI(0, latencyMs);
        ui.txt_control_log->appendPlainText(
            QString(">> Route to %1: %2").arg(targetPort).arg(QString::fromUtf8(command)));
    });
    connect(m_runtime, &relay::RelayRuntime::robotReplyObserved,
            this, [this](int targetPort, const QByteArray& reply, double latencyMs) {
        updateProcessLatencyUI(3, latencyMs);
        ui.txt_feedback_log->appendPlainText(
            QString(">> Robot %1 to Relay: %2").arg(targetPort).arg(QString::fromUtf8(reply)));
    });

    m_udpTwinSocket = new QUdpSocket(this);
    connect(m_udpTwinSocket, &QUdpSocket::readyRead, this, &RelayStation::onUdpTwinReadyRead);

    m_udpForceSocket = new QUdpSocket(this);
    if (m_udpForceSocket->bind(QHostAddress::Any, 8887, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        ui.txt_state_log->appendPlainText(">> Force UDP server started, port: 8887");
    }
    else {
        ui.txt_state_log->appendPlainText(QString(">> [Error] Force UDP port 8887 bind failed: %1").arg(m_udpForceSocket->errorString()));
    }
    connect(m_udpForceSocket, &QUdpSocket::readyRead, this, &RelayStation::onUdpForceReadyRead);

    m_twinHeartbeatTimer = new QTimer(this);
    m_twinHeartbeatTimer->setSingleShot(true);
    connect(m_twinHeartbeatTimer, &QTimer::timeout, this, [this]() {
        ui.txt_state_log->appendPlainText(">> [Twin Stream] UDP hole punch timeout, stopping transmission.");
        m_twinTargetIp.clear();
        m_twinTargetPort = 0;
        m_streamDiagnostics.twinSubscribed = false;
        syncStreamDiagnostics();

        });

    m_forceHeartbeatTimer = new QTimer(this);
    m_forceHeartbeatTimer->setSingleShot(true);
    connect(m_forceHeartbeatTimer, &QTimer::timeout, this, [this]() {
        ui.txt_state_log->appendPlainText(">> [Force Stream] UDP heartbeat timeout, stopping transmission.");
        m_forceTargetIp.clear();
        m_forceTargetPort = 0;
        m_streamDiagnostics.forceSubscribed = false;
        syncStreamDiagnostics();
        });

    m_udpCommandSocket = new QUdpSocket(this);
    m_udpCommandSocket->bind(8886, QUdpSocket::ShareAddress);
    connect(m_udpCommandSocket, &QUdpSocket::readyRead, this, &RelayStation::onUdpCommandReadyRead);

    m_videoHeartbeatTimer = new QTimer(this);
    m_videoHeartbeatTimer->setSingleShot(true);
    connect(m_videoHeartbeatTimer, &QTimer::timeout, this, [this]() {
        if (m_videoThread) m_videoThread->stopStreaming();
        m_videoTargetIp.clear();
        m_videoTargetPort = 0;
        m_streamDiagnostics.videoSubscribed = false;
        syncStreamDiagnostics();
        if (ui.relay_video_status) {
            ui.relay_video_status->setText("Timeout/Stopped");
            ui.relay_video_status->setStyleSheet("color: #FFD166;");
        }
        });
}

RelayStation::~RelayStation()
{
    if ((m_runtime && m_runtime->isRunning()) || m_latencyThread) {
        on_btn_stop_clicked();
    }
}

void RelayStation::on_btn_start_clicked() {
    QString robot_ip = ui.edit_robot_ip->text();
    int motion_port = ui.edit_motion_port->text().toInt();
    int enable_port = ui.edit_enable_port->text().toInt();
    int relay_port = ui.edit_relay_port_control->text().toInt();
    int digitaltwin_port = ui.edit_relay_port_digitaltwin->text().toInt();
    int realtime_port = ui.edit_realtime_port->text().toInt();

    ui.txt_state_log->appendPlainText("===== Starting Embodied AI Relay Station =====");

    if (!m_latencyThread) {
        m_latencyThread = new QThread();
        m_worker = new PingWorker();
        m_worker->timer = &m_netTestTimer;
        if (ui.combo_payload_type) m_worker->currentPayloadIndex = ui.combo_payload_type->currentIndex();

        m_worker->moveToThread(m_latencyThread);

        connect(m_latencyThread, &QThread::started, m_worker, &PingWorker::setup);
        connect(m_latencyThread, &QThread::finished, m_worker, &QObject::deleteLater);
        connect(m_worker, &PingWorker::resultReady, this, &RelayStation::updateLatencyFromWorker);
        connect(m_worker, &PingWorker::logToUi, ui.txt_state_log, &QPlainTextEdit::appendPlainText);
        connect(m_worker, &PingWorker::pongReceived, this, [this]() { m_isWaitingPONG = false; });

        m_latencyThread->start();
    }

    if (m_udpTwinSocket->state() != QAbstractSocket::BoundState) {
        if (m_udpTwinSocket->bind(QHostAddress::Any, digitaltwin_port, QUdpSocket::ShareAddress)) {
            ui.txt_state_log->appendPlainText(QString(">> Twin UDP server started, port: %1").arg(digitaltwin_port));
        }
    }

    if (!m_runtime->start(robot_ip,
                          static_cast<quint16>(enable_port),
                          static_cast<quint16>(motion_port),
                          static_cast<quint16>(realtime_port),
                          static_cast<quint16>(relay_port),
                          relay::kV2Port)) {
        ui.txt_state_log->appendPlainText(">> [Error] Relay startup aborted; no robot commands were sent.");
        on_btn_stop_clicked();
        return;
    }

    ui.btn_start->setEnabled(false);
    m_netTimer->start(500);
}


void RelayStation::on_btn_stop_clicked() {
    if (m_netTimer && m_netTimer->isActive()) m_netTimer->stop();
    m_isWaitingPONG = false;

    ui.txt_state_log->appendPlainText("===== Stopping Embodied AI Relay Station =====");

    if (m_latencyThread) {
        QMetaObject::invokeMethod(m_worker, "stopAll", Qt::BlockingQueuedConnection);
        m_latencyThread->quit();
        m_latencyThread->wait();
        delete m_latencyThread;
        m_worker = nullptr; m_latencyThread = nullptr;
    }

    if (m_runtime) m_runtime->stop();
    stopCompatibilityStreams();

    if (ui.relay_video_status) {
        ui.relay_video_status->setText("Closed");
        ui.relay_video_status->setStyleSheet("color: #A0A0A0;");
    }

    if (ui.robot_30004_status) {
        ui.robot_30004_status->setText("Closed");
        ui.robot_30004_status->setStyleSheet("color: #A0A0A0;");
    }

    ui.LED_Touch->setStyleSheet("QLabel { background-color: #A0A0A0; border-radius: 8px; border: 1px solid #888888; }");
    ui.LED_Robot->setStyleSheet("QLabel { background-color: #A0A0A0; border-radius: 8px; border: 1px solid #888888; }");

    // 🚀 【新增】：全局停止时，灯也要变灰
    ui.LED_Camera->setStyleSheet("QLabel { background-color: #A0A0A0; border-radius: 8px; border: 1px solid #888888; }");

    ui.btn_start->setEnabled(true);
    ui.txt_state_log->appendPlainText("===== Relay Station Fully Closed =====");
}

void RelayStation::updateLatencyFromWorker(double latencyMs) {
    updateNetworkLatencyUI(latencyMs);
}

void RelayStation::onUdpTwinReadyRead() {
    while (m_udpTwinSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(int(m_udpTwinSocket->pendingDatagramSize()));
        QHostAddress senderIp;
        quint16 senderPort;
        m_udpTwinSocket->readDatagram(datagram.data(), datagram.size(), &senderIp, &senderPort);

        if (!m_runtime || !m_runtime->isRunning()) continue;
        if (senderIp.protocol() == QAbstractSocket::IPv6Protocol) senderIp = QHostAddress(senderIp.toIPv4Address());

        QString cmd = QString::fromUtf8(datagram).trimmed();
        if (cmd == "TWIN_START") {
            if (m_twinTargetIp != senderIp || m_twinTargetPort != senderPort) {
                ui.txt_state_log->appendPlainText(QString(">> [Twin UDP successful] Mapped address: %1:%2").arg(senderIp.toString()).arg(senderPort));
            }
            m_twinTargetIp = senderIp;
            m_twinTargetPort = senderPort;
            m_twinHeartbeatTimer->start(5000);
            m_streamDiagnostics.twinSubscribed = true;
            syncStreamDiagnostics();

        }
        else if (cmd == "TWIN_STOP") {
            m_twinHeartbeatTimer->stop();
            m_twinTargetIp.clear();
            m_twinTargetPort = 0;
            m_streamDiagnostics.twinSubscribed = false;
            syncStreamDiagnostics();
            ui.txt_state_log->appendPlainText(">> [Twin Stream] Touch triggered STOP. Ceasing data transmission.");

        }
    }
}

void RelayStation::onUdpForceReadyRead() {
    while (m_udpForceSocket && m_udpForceSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(int(m_udpForceSocket->pendingDatagramSize()));
        QHostAddress senderIp;
        quint16 senderPort;
        m_udpForceSocket->readDatagram(datagram.data(), datagram.size(), &senderIp, &senderPort);

        if (!m_runtime || !m_runtime->isRunning()) continue;
        if (senderIp.protocol() == QAbstractSocket::IPv6Protocol) senderIp = QHostAddress(senderIp.toIPv4Address());

        QString cmd = QString::fromUtf8(datagram).trimmed();
        if (cmd == "FORCE_START") {
            if (m_forceTargetIp != senderIp || m_forceTargetPort != senderPort) {
                ui.txt_state_log->appendPlainText(QString(">> [Force UDP successful] Mapped address: %1:%2").arg(senderIp.toString()).arg(senderPort));
            }
            m_forceTargetIp = senderIp;
            m_forceTargetPort = senderPort;
            m_forceHeartbeatTimer->start(2000);
            m_streamDiagnostics.forceSubscribed = true;
            syncStreamDiagnostics();
        }
        else if (cmd == "FORCE_STOP") {
            if (m_forceHeartbeatTimer) m_forceHeartbeatTimer->stop();
            m_forceTargetIp.clear();
            m_forceTargetPort = 0;
            m_streamDiagnostics.forceSubscribed = false;
            syncStreamDiagnostics();
            ui.txt_state_log->appendPlainText(">> [Force Stream] Touch triggered STOP. Ceasing force transmission.");
        }
    }
}


void RelayStation::onRobotStateAvailable()
{
    if (!m_runtime) return;
    const relay::RobotStateSnapshot state = m_runtime->latestState();
    if (!state.valid) return;

    if (!m_twinTargetIp.isNull() && m_udpTwinSocket) {
        const QByteArray packet = relay::twinPacket(state).toUtf8();
        if (!packet.isEmpty() &&
            m_udpTwinSocket->writeDatagram(packet, m_twinTargetIp, m_twinTargetPort) == packet.size()) {
            ++m_streamDiagnostics.twinSent;
        }
    }
    if (!m_forceTargetIp.isNull() && m_udpForceSocket) {
        const QByteArray packet = relay::forcePacket(state).toUtf8();
        if (!packet.isEmpty() &&
            m_udpForceSocket->writeDatagram(packet, m_forceTargetIp, m_forceTargetPort) == packet.size()) {
            ++m_streamDiagnostics.forceSent;
        }
    }
    syncStreamDiagnostics();
}


void RelayStation::onUdpCommandReadyRead()
{
    while (m_udpCommandSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<int>(m_udpCommandSocket->pendingDatagramSize()));
        QHostAddress senderIp;
        quint16 senderPort = 0;
        m_udpCommandSocket->readDatagram(datagram.data(), datagram.size(), &senderIp, &senderPort);
        if (!m_runtime || !m_runtime->isRunning()) continue;
        if (senderIp.protocol() == QAbstractSocket::IPv6Protocol) {
            senderIp = QHostAddress(senderIp.toIPv4Address());
        }
        const QString command = QString::fromUtf8(datagram).trimmed();
        if (command == QStringLiteral("VIDEO_START")) {
            const bool subscriberChanged = m_videoTargetIp != senderIp || m_videoTargetPort != senderPort;
            if (subscriberChanged && m_videoThread) {
                m_videoThread->stopStreaming();
            }
            if (subscriberChanged ||
                !m_videoHeartbeatTimer->isActive()) {
                ui.txt_state_log->appendPlainText(
                    QString(">> [Video UDP] Subscriber: %1:%2")
                        .arg(senderIp.toString()).arg(senderPort));
            }
            m_videoTargetIp = senderIp;
            m_videoTargetPort = senderPort;
            m_streamDiagnostics.videoSubscribed = true;
            ensureVideoThread();
            m_videoThread->startStreaming();
            m_videoHeartbeatTimer->start(5000);
            syncStreamDiagnostics();
            ui.relay_video_status->setText("UDP Streaming");
            ui.relay_video_status->setStyleSheet("color: #FFD166;");
        } else if (command == QStringLiteral("VIDEO_STOP")) {
            if (m_videoThread) m_videoThread->stopStreaming();
            m_videoHeartbeatTimer->stop();
            m_videoTargetIp.clear();
            m_videoTargetPort = 0;
            m_streamDiagnostics.videoSubscribed = false;
            syncStreamDiagnostics();
            ui.txt_state_log->appendPlainText(">> [Video Stream] Touch triggered STOP; queue cleared.");
            ui.relay_video_status->setText("Stopped");
            ui.relay_video_status->setStyleSheet("color: #A0A0A0;");
            ui.LED_Camera->setStyleSheet(
                "QLabel { background-color: #A0A0A0; border-radius: 8px; border: 1px solid #888888; }");
        }
    }
}

void RelayStation::ensureVideoThread()
{
    if (m_videoThread) return;
    m_videoThread = new VideoStreamThread(this);
    connect(m_videoThread, &VideoStreamThread::frameAvailable, this, [this] {
        const std::optional<relay::EncodedCameraFrame> frame = m_videoThread->takeLatestFrame();
        if (!frame.has_value() || !m_runtime || !m_runtime->isRunning() ||
            !m_videoHeartbeatTimer->isActive() || m_videoTargetIp.isNull()) {
            ++m_cameraDispatchDrops;
            syncStreamDiagnostics();
            return;
        }
        relay::EncodedCameraFrame outgoing = *frame;
        outgoing.metadata.sendSteadyNs = relay::steadyNowNs();
        const qint64 sent = m_udpCommandSocket->writeDatagram(
            outgoing.bytes, m_videoTargetIp, m_videoTargetPort);
        if (sent == outgoing.bytes.size()) {
            ++m_streamDiagnostics.videoSent;
            m_streamDiagnostics.cameraFrameSeq = outgoing.metadata.frameSeq;
            m_streamDiagnostics.cameraLastCaptureSteadyNs = outgoing.metadata.captureSteadyNs;
            m_streamDiagnostics.cameraLastEncodedSteadyNs = outgoing.metadata.encodedSteadyNs;
            m_streamDiagnostics.cameraLastSendSteadyNs = outgoing.metadata.sendSteadyNs;
            m_streamDiagnostics.cameraLastBytes = outgoing.metadata.byteCount;
            m_streamDiagnostics.cameraLastWidth = outgoing.metadata.width;
            m_streamDiagnostics.cameraLastHeight = outgoing.metadata.height;
            m_streamDiagnostics.cameraLastEncoding = outgoing.metadata.encoding;
            const double processingMs = static_cast<double>(
                outgoing.metadata.encodedSteadyNs - outgoing.metadata.captureSteadyNs) / 1.0e6;
            updateProcessLatencyUI(2, processingMs);
        } else {
            ++m_streamDiagnostics.cameraSendFailures;
        }
        syncStreamDiagnostics();
    });
    connect(m_videoThread, &VideoStreamThread::cameraStatusChanged, this, [this](bool open) {
        ui.LED_Camera->setStyleSheet(open
            ? "QLabel { background-color: #00FF00; border-radius: 8px; border: 1px solid #00AA00; }"
            : "QLabel { background-color: #FF0000; border-radius: 8px; border: 1px solid #AA0000; }");
        ui.txt_state_log->appendPlainText(open
            ? ">> [Video Stream] Camera opened."
            : ">> [Error] Camera open failed; no video payload was produced.");
        syncStreamDiagnostics();
    });
}

void RelayStation::syncStreamDiagnostics()
{
    if (m_videoThread) {
        m_streamDiagnostics.cameraCaptureFailures = m_videoThread->captureFailures();
        m_streamDiagnostics.cameraEncodeFailures = m_videoThread->encodeFailures();
        m_streamDiagnostics.cameraDrops = m_cameraDispatchDrops + m_videoThread->droppedFrames();
    }
    if (m_runtime) m_runtime->setStreamDiagnostics(m_streamDiagnostics);
}

void RelayStation::stopCompatibilityStreams()
{
    if (m_twinHeartbeatTimer) m_twinHeartbeatTimer->stop();
    if (m_forceHeartbeatTimer) m_forceHeartbeatTimer->stop();
    if (m_videoHeartbeatTimer) m_videoHeartbeatTimer->stop();
    m_twinTargetIp.clear();
    m_twinTargetPort = 0;
    m_forceTargetIp.clear();
    m_forceTargetPort = 0;
    m_videoTargetIp.clear();
    m_videoTargetPort = 0;
    if (m_videoThread) m_videoThread->stopStreaming();
    m_streamDiagnostics.twinSubscribed = false;
    m_streamDiagnostics.forceSubscribed = false;
    m_streamDiagnostics.videoSubscribed = false;
    syncStreamDiagnostics();
}

void RelayStation::updateRobotIndicator()
{
    if (!m_runtime) return;
    relay::RelayStatusInput input;
    input.port29999Connected = m_runtime->portConnected(relay::kDashboardPort);
    input.port30003Connected = m_runtime->portConnected(relay::kMotionPort);
    input.port30004Connected = m_runtime->portConnected(relay::kFeedbackPort);
    input.nowSteadyNs = relay::steadyNowNs();
    input.state = m_runtime->latestState();
    if (relay::relayReady(input)) {
        ui.LED_Robot->setStyleSheet(
            "QLabel { background-color: #00FF00; border-radius: 8px; border: 1px solid #00AA00; }");
        ui.robot_30004_status->setText("Relay Ready");
        ui.robot_30004_status->setStyleSheet("color: #00AA00;");
    } else if (input.port29999Connected || input.port30003Connected || input.port30004Connected) {
        ui.LED_Robot->setStyleSheet(
            "QLabel { background-color: #FFD166; border-radius: 8px; border: 1px solid #CC9A00; }");
        ui.robot_30004_status->setText("Connected / Not Ready");
        ui.robot_30004_status->setStyleSheet("color: #FFD166;");
    } else {
        ui.LED_Robot->setStyleSheet(
            "QLabel { background-color: #FF0000; border-radius: 8px; border: 1px solid #AA0000; }");
        ui.robot_30004_status->setText("Disconnected");
        ui.robot_30004_status->setStyleSheet("color: #FF3B30;");
    }
}

void RelayStation::initProcessLatencyPlot() {
    ui.plot_relay_latency->setBackground(QBrush(QColor("#1B212D")));
    QPen whiteAxisPen(QColor("#E2E8F0"));
    ui.plot_relay_latency->xAxis->setBasePen(whiteAxisPen);
    ui.plot_relay_latency->xAxis->setTickPen(whiteAxisPen);
    ui.plot_relay_latency->xAxis->setSubTickPen(whiteAxisPen);
    ui.plot_relay_latency->xAxis->setTickLabelColor(QColor("#E2E8F0"));
    ui.plot_relay_latency->xAxis->setLabelColor(QColor("#E2E8F0"));
    ui.plot_relay_latency->yAxis->setBasePen(whiteAxisPen);
    ui.plot_relay_latency->yAxis->setTickPen(whiteAxisPen);
    ui.plot_relay_latency->yAxis->setSubTickPen(whiteAxisPen);
    ui.plot_relay_latency->yAxis->setTickLabelColor(QColor("#E2E8F0"));
    ui.plot_relay_latency->yAxis->setLabelColor(QColor("#E2E8F0"));
    ui.plot_relay_latency->xAxis->grid()->setPen(QPen(QColor("#31394E"), 1, Qt::DotLine));
    ui.plot_relay_latency->yAxis->grid()->setPen(QPen(QColor("#31394E"), 1, Qt::DotLine));
    ui.plot_relay_latency->legend->setBrush(QBrush(QColor(27, 33, 45, 220)));
    ui.plot_relay_latency->legend->setBorderPen(QPen(QColor("#454E65"), 1));
    ui.plot_relay_latency->legend->setTextColor(QColor("#E2E8F0"));
    ui.plot_relay_latency->xAxis->setLabel("Processing Count");
    ui.plot_relay_latency->yAxis->setLabel("Latency (ms)");

    QFont axisLabelFont("Segoe UI", 12, QFont::Bold);
    QFont tickLabelFont("Segoe UI", 10);
    ui.plot_relay_latency->xAxis->setLabelFont(axisLabelFont);
    ui.plot_relay_latency->yAxis->setLabelFont(axisLabelFont);
    ui.plot_relay_latency->xAxis->setTickLabelFont(tickLabelFont);
    ui.plot_relay_latency->yAxis->setTickLabelFont(tickLabelFont);

    ui.plot_relay_latency->addGraph();
    ui.plot_relay_latency->graph(0)->setName("Control (Touch -> Robot)");
    QPen penControl(QColor("#9D4EDD")); // Purple
    penControl.setWidth(2);
    ui.plot_relay_latency->graph(0)->setPen(penControl);

    ui.plot_relay_latency->addGraph();
    ui.plot_relay_latency->graph(1)->setName("Digital Twin");
    QPen penTwin(QColor("#00F5D4"));    // Mint Green
    penTwin.setWidth(2);
    ui.plot_relay_latency->graph(1)->setPen(penTwin);

    ui.plot_relay_latency->addGraph();
    ui.plot_relay_latency->graph(2)->setName("Video");
    QPen penVideo(QColor("#FEE440"));   // Yellow
    penVideo.setWidth(2);
    ui.plot_relay_latency->graph(2)->setPen(penVideo);

    ui.plot_relay_latency->addGraph();
    ui.plot_relay_latency->graph(3)->setName("Feedback (Robot -> Touch)");
    QPen penFeedback(QColor("#00D2FF")); // Light Blue
    penFeedback.setWidth(2);
    ui.plot_relay_latency->graph(3)->setPen(penFeedback);

    ui.plot_relay_latency->legend->setVisible(false); // 初始隐藏，由自动更新逻辑接管
    QFont legendFont("Segoe UI", 10);
    legendFont.setWeight(QFont::Medium);
    ui.plot_relay_latency->legend->setFont(legendFont);
    ui.plot_relay_latency->legend->setBrush(QBrush(QColor(27, 33, 45, 230)));
    ui.plot_relay_latency->legend->setBorderPen(QPen(QColor("#454E65"), 1));
    ui.plot_relay_latency->legend->setIconSize(35, 12);
    ui.plot_relay_latency->legend->setRowSpacing(2);
    ui.plot_relay_latency->legend->setColumnSpacing(8);
    ui.plot_relay_latency->legend->setMargins(QMargins(8, 6, 8, 6));
    ui.plot_relay_latency->axisRect()->insetLayout()->setInsetAlignment(0, Qt::AlignTop | Qt::AlignRight);

    // 默认 Control 状态的图例
    ui.plot_relay_latency->graph(1)->removeFromLegend();
    ui.plot_relay_latency->graph(2)->removeFromLegend();
    ui.plot_relay_latency->replot();
}

void RelayStation::updateProcessLatencyUI(int channelIndex, double latencyMs) {
    // 🚀 核心机制 3：未选中时强制返回，既丢弃历史数据防后台累积，又实现了暂停冻结效果
    if (channelIndex == 0 || channelIndex == 3) {
        if (!ui.chk_show_control->isChecked()) return;
    }
    else if (channelIndex == 1) {
        if (!ui.chk_show_digitaltwin->isChecked()) return;
    }
    else if (channelIndex == 2) {
        if (!ui.chk_show_video->isChecked()) return;
    }

    m_latencyHistory[channelIndex].append(latencyMs);
    while (m_latencyHistory[channelIndex].size() > 20) {
        m_latencyHistory[channelIndex].removeFirst();
    }

    int currentX = 0;
    if (channelIndex == 0) currentX = ++m_packetCountControl;
    else if (channelIndex == 1) currentX = ++m_packetCountTwin;
    else if (channelIndex == 2) currentX = ++m_packetCountVideo;
    else if (channelIndex == 3) currentX = ++m_packetCountFeedback;

    ui.plot_relay_latency->graph(channelIndex)->addData(currentX, latencyMs);

    if (channelIndex == 0 || channelIndex == 1 || channelIndex == 2) {
        double sum = 0;
        for (double val : m_latencyHistory[channelIndex]) sum += val;
        double avg = (m_latencyHistory[channelIndex].size() > 0) ? (sum / m_latencyHistory[channelIndex].size()) : 0;
        ui.avg_relay_latency->setText(QString::number(avg, 'f', 3));
    }

    double maxInWindow = 0.15;
    if (ui.chk_show_control->isChecked()) {
        for (double v : m_latencyHistory[0]) if (v * 2.0 > maxInWindow) maxInWindow = v * 2.0;
        for (double v : m_latencyHistory[3]) if (v * 2.0 > maxInWindow) maxInWindow = v * 2.0;
    }
    else {
        for (double v : m_latencyHistory[channelIndex]) {
            if (v * 2.0 > maxInWindow) maxInWindow = v * 2.0;
        }
    }
    ui.plot_relay_latency->yAxis->setRange(0, maxInWindow);

    int refX = currentX;
    if (ui.chk_show_control->isChecked()) refX = m_packetCountControl;
    else if (ui.chk_show_digitaltwin->isChecked()) refX = m_packetCountTwin;
    else if (ui.chk_show_video->isChecked()) refX = m_packetCountVideo;

    if (refX > 100) ui.plot_relay_latency->xAxis->setRange(refX - 100, refX);
    else ui.plot_relay_latency->xAxis->setRange(0, 100);

    ui.plot_relay_latency->replot();
}

void RelayStation::updateNetworkLatencyUI(double netLatency) {
    m_networkLatencyHistory.append(netLatency);
    while (m_networkLatencyHistory.size() > 20) {
        m_networkLatencyHistory.removeFirst();
    }
    int currentX = ++m_packetCountNetwork;
    ui.plot_network_latency->graph(0)->addData(currentX, netLatency);

    if (ui.group_network_latency->isChecked()) {
        double sum = 0;
        for (double val : m_networkLatencyHistory) sum += val;
        if (!m_networkLatencyHistory.isEmpty()) {
            double avg = sum / m_networkLatencyHistory.size();
            ui.avg_network_latency->setText(QString::number(avg, 'f', 3));
        }
        double maxInWindow = 5.0;
        for (double v : m_networkLatencyHistory) {
            if (v * 2.0 > maxInWindow) maxInWindow = v * 2.0;
        }
        ui.plot_network_latency->yAxis->setRange(0, maxInWindow);
        if (currentX > 100) {
            ui.plot_network_latency->xAxis->setRange(currentX - 100, currentX);
        }
        else {
            ui.plot_network_latency->xAxis->setRange(0, 100);
        }
        ui.plot_network_latency->replot();
    }
}
