#include "relay_station.h"
#include <QDebug> 
#include <QNetworkProxy>


RelayStation::RelayStation(QWidget* parent)
    : QMainWindow(parent)
{
    ui.setupUi(this);
    initProcessLatencyPlot();
    ui.group_network_latency->setChecked(false);

    connect(ui.combo_payload_type, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        m_networkLatencyHistory.clear();
        });

    m_netTestTimer.start();
    m_netTimer = new QTimer(this);
    static qint64 lastPingTimeMs = 0;
    static QByteArray txBuffer;
    static int lastPayloadSize = -1;

    connect(m_netTimer, &QTimer::timeout, this, [this]() {
        if (m_isWaitingPONG && (m_netTestTimer.elapsed() - lastPingTimeMs) > 3000) {
            m_isWaitingPONG = false;
        }
        if (ui.group_network_latency->isChecked() && m_client && m_client->isOpen() && !m_isWaitingPONG) {
            int payloadSize = 32;
            if (ui.combo_payload_type) {
                int index = ui.combo_payload_type->currentIndex();
                if (index == 1) payloadSize = 1024 * 50;
                else if (index == 2) payloadSize = 1024 * 1024 * 3;
            }
            if (payloadSize != lastPayloadSize) {
                txBuffer.resize(payloadSize + 64);
                memset(txBuffer.data(), 'X', payloadSize);
                lastPayloadSize = payloadSize;
            }
            qint64 currentNsecs = m_netTestTimer.nsecsElapsed();
            char tailStr[64];
            int tailLen = sprintf_s(tailStr, sizeof(tailStr), "PING|%lld|", currentNsecs);
            memcpy(txBuffer.data() + payloadSize, tailStr, tailLen);

            m_isWaitingPONG = true;
            lastPingTimeMs = m_netTestTimer.elapsed();
            m_client->write(txBuffer.constData(), payloadSize + tailLen);
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

    ui.chk_show_control->setAutoExclusive(false);
    ui.chk_show_tactile->setAutoExclusive(false);
    ui.chk_show_video->setAutoExclusive(false);

    connect(ui.chk_show_control, &QAbstractButton::clicked, this, [=](bool checked) {
        if (checked) { ui.chk_show_tactile->setChecked(false); ui.chk_show_video->setChecked(false); }
        });
    connect(ui.chk_show_tactile, &QAbstractButton::clicked, this, [=](bool checked) {
        if (checked) { ui.chk_show_control->setChecked(false); ui.chk_show_video->setChecked(false); }
        });
    connect(ui.chk_show_video, &QAbstractButton::clicked, this, [=](bool checked) {
        if (checked) { ui.chk_show_control->setChecked(false); ui.chk_show_tactile->setChecked(false); }
        });

    connect(ui.chk_show_control, &QCheckBox::toggled, this, [=](bool checked) {
        if (ui.plot_relay_latency->graphCount() > 0) ui.plot_relay_latency->graph(0)->setVisible(checked);
        if (ui.plot_relay_latency->graphCount() > 3) ui.plot_relay_latency->graph(3)->setVisible(checked);
        ui.plot_relay_latency->legend->setVisible(checked);
        ui.plot_relay_latency->replot();
        });
    connect(ui.chk_show_tactile, &QCheckBox::toggled, this, [=](bool checked) {
        if (ui.plot_relay_latency->graphCount() > 1) { ui.plot_relay_latency->graph(1)->setVisible(checked); ui.plot_relay_latency->replot(); }
        });
    connect(ui.chk_show_video, &QCheckBox::toggled, this, [=](bool checked) {
        if (ui.plot_relay_latency->graphCount() > 2) { ui.plot_relay_latency->graph(2)->setVisible(checked); ui.plot_relay_latency->replot(); }
        });

    // ==========================================================
    // 🚀 服务器与 Socket 初始化
    // ==========================================================
    m_server = new QTcpServer(this);
    m_client = nullptr;

    m_statusServer = new QTcpServer(this);
    m_robotStatusSocket = new QTcpSocket(this);
    m_robotStatusSocket->setProxy(QNetworkProxy::NoProxy);

    connect(m_statusServer, &QTcpServer::newConnection, this, &RelayStation::onNewStatusConnection);
    connect(m_robotStatusSocket, &QTcpSocket::readyRead, this, &RelayStation::onRobotStatusReadyRead);

    // 🚀 新增：30004 端口连接成功的日志打印
    connect(m_robotStatusSocket, &QTcpSocket::connected, this, [this]() {
        ui.txt_state_log->appendPlainText(">> Robot status port 30004 connected ");
        // 点亮状态灯（如果你希望 30004 也能触发机器人的绿灯）
        ui.LED_Robot->setStyleSheet("QLabel { background-color: #00FF00; border-radius: 8px; border: 1px solid #00AA00; }");
        });

    // 🚀 修复版：30004 端口异常或断开的日志打印 (采用最新的 errorOccurred 信号)
    connect(m_robotStatusSocket, &QTcpSocket::errorOccurred, this, [this](QTcpSocket::SocketError socketError) {
        ui.txt_state_log->appendPlainText(QString(">> [Error] Robot port 30004 disconnected or failed: %1").arg(m_robotStatusSocket->errorString()));
        ui.LED_Robot->setStyleSheet("QLabel { background-color: #FF0000; border-radius: 8px; border: 1px solid #AA0000; }");
        });

    connect(m_server, &QTcpServer::newConnection, this, [this]() {
        if (this->m_client) {
            this->m_client->disconnectFromHost();
            this->m_client->deleteLater();
        }
        this->m_client = m_server->nextPendingConnection();

        ui.LED_Touch->setStyleSheet("QLabel { background-color: #00FF00; border-radius: 8px; border: 1px solid #00AA00; }");
        ui.txt_state_log->appendPlainText(">> [Connected] Touch client has joined the relay station!");

        connect(this->m_client, &QTcpSocket::disconnected, this, [this]() {
            ui.LED_Touch->setStyleSheet("QLabel { background-color: #FF0000; border-radius: 8px; border: 1px solid #AA0000; }");
            ui.txt_state_log->appendPlainText(">> [Disconnected] Touch client lost!");
            if (this->m_client) {
                this->m_client->deleteLater();
                this->m_client = nullptr;
            }
            });

        connect(this->m_client, &QTcpSocket::errorOccurred, this, [this](QTcpSocket::SocketError socketError) {
            if (socketError != QTcpSocket::RemoteHostClosedError) {
                ui.txt_state_log->appendPlainText(">> [Error] Touch connection abnormally terminated!");
                ui.LED_Touch->setStyleSheet("QLabel { background-color: #FF0000; border-radius: 8px; border: 1px solid #AA0000; }");
            }
            });

        connect(this->m_client, &QTcpSocket::readyRead, this, [this]() {
            QElapsedTimer processTimer;
            processTimer.start();
            QByteArray data = this->m_client->readAll();

            if (ui.group_network_latency->isChecked()) {
                int pongIdx = data.lastIndexOf("PONG|");
                if (pongIdx != -1) {
                    QString pongPart = QString::fromUtf8(data.mid(pongIdx));
                    QStringList parts = pongPart.split('|');
                    if (parts.size() >= 3) {
                        qint64 sentNsecs = parts[1].toLongLong();
                        if (sentNsecs > 0) {
                            qint64 currentNsecs = m_netTestTimer.nsecsElapsed();
                            double rttMs = (currentNsecs - sentNsecs) / 1000000.0;
                            m_isWaitingPONG = false;
                            updateNetworkLatencyUI(rttMs / 2.0);
                        }
                    }
                }
            }

            int idx30003 = data.indexOf("30003|");
            int idx29999 = data.indexOf("29999|");
            int targetIdx = (idx30003 != -1 && idx29999 != -1) ? qMax(idx30003, idx29999) : qMax(idx30003, idx29999);

            if (targetIdx != -1) {
                QByteArray cmdData = data.mid(targetIdx);
                int separatorIdx = cmdData.indexOf('|');
                if (separatorIdx != -1) {
                    int targetPort = cmdData.left(separatorIdx).toInt();
                    QByteArray cmdBody = cmdData.mid(separatorIdx + 1);

                    if (targetPort == 29999 && m_robot_enable && m_robot_enable->isOpen()) {
                        m_robot_enable->write(cmdBody);
                    }
                    else if (targetPort == 30003 && m_robot_motion && m_robot_motion->isOpen()) {
                        m_robot_motion->write(cmdBody);
                    }

                    double relayLatencyMs = processTimer.nsecsElapsed() / 1000000.0;
                    updateProcessLatencyUI(0, relayLatencyMs);

                    QString cmdText = QString::fromUtf8(cmdBody);
                    ui.txt_control_log->appendPlainText(QString(">> Route to %1: %2").arg(targetPort).arg(cmdText));
                }
            }
            });
        });

    // ==========================================================
    // 🚀 新增：初始化 30004 端口状态超时检测定时器
    // ==========================================================
    m_statusTimeoutTimer = new QTimer(this);
    m_statusTimeoutTimer->setSingleShot(true); // 设置为单次触发

    // 如果 500ms 没有重置定时器，说明没收到新数据，清空显示
    connect(m_statusTimeoutTimer, &QTimer::timeout, this, [this]() {
        if (ui.robot_30004_status) {
            ui.robot_30004_status->setText("");
        }
        });
}

RelayStation::~RelayStation() {}

void RelayStation::on_btn_start_clicked() {
    QString robot_ip = ui.edit_robot_ip->text();
    int motion_port = ui.edit_motion_port->text().toInt();
    int enable_port = ui.edit_enable_port->text().toInt();
    int relay_port = ui.edit_relay_port_control->text().toInt();

    // 🚀 新增：从 UI 界面读取 8889 和 30004 对应的端口号
    int digitaltwin_port = ui.edit_relay_port_digitaltwin->text().toInt();
    int realtime_port = ui.edit_realtime_port->text().toInt();

    ui.txt_state_log->appendPlainText("===== Starting Embodied AI Relay Station =====");

    if (!m_server->isListening()) {
        if (m_server->listen(QHostAddress::Any, relay_port)) {
            ui.txt_state_log->appendPlainText(QString(">> Relay server started, listening on port: %1").arg(relay_port));
        }
    }

    // 🚀 修改：使用从 UI 读取的数字孪生端口（代替原来的 8889）
    if (!m_statusServer->isListening()) {
        if (m_statusServer->listen(QHostAddress::Any, digitaltwin_port)) {
            ui.txt_state_log->appendPlainText(QString(">> Status server started, listening on port: %1").arg(digitaltwin_port));
        }
    }

    // 🚀 修改：把读取到的 realtime_port 传给初始化函数
    initRobotConnections(robot_ip, motion_port, enable_port, realtime_port);

    ui.btn_start->setEnabled(false);
    m_netTimer->start(500);
}

void RelayStation::initRobotConnections(QString ip, int motionPort, int enablePort, int realtimePort) {
    m_robot_motion = new QTcpSocket(this);
    m_robot_motion->setProxy(QNetworkProxy::NoProxy);
    m_robot_enable = new QTcpSocket(this);
    m_robot_enable->setProxy(QNetworkProxy::NoProxy);

    connect(m_robot_motion, &QTcpSocket::connected, this, [=]() {
        ui.txt_state_log->appendPlainText(QString(">> Robot motion port %1 connected").arg(motionPort));
        ui.LED_Robot->setStyleSheet("QLabel { background-color: #00FF00; border-radius: 8px; border: 1px solid #00AA00; }");
        });

    connect(m_robot_motion, &QTcpSocket::errorOccurred, this, [=](QTcpSocket::SocketError socketError) {
        ui.txt_state_log->appendPlainText(">> [Error] Motion Port 30003 Failed: " + m_robot_motion->errorString());
        ui.LED_Robot->setStyleSheet("QLabel { background-color: #FF0000; border-radius: 8px; border: 1px solid #AA0000; }");
        });

    connect(m_robot_motion, &QTcpSocket::readyRead, this, [=]() {
        QElapsedTimer processTimer;
        processTimer.start();
        QByteArray data = m_robot_motion->readAll();

        if (m_client && m_client->isOpen()) {
            m_client->write(data);
        }

        double relayLatencyMs = processTimer.nsecsElapsed() / 1000000.0;
        updateProcessLatencyUI(3, relayLatencyMs);
        ui.txt_feedback_log->appendPlainText(">> Robot 30003 to Relay: " + data);
        });

    m_robot_motion->connectToHost(ip, motionPort);

    connect(m_robot_enable, &QTcpSocket::connected, this, [=]() {
        ui.txt_state_log->appendPlainText(QString(">> Robot enable port %1 connected").arg(enablePort));
        });

    connect(m_robot_enable, &QTcpSocket::errorOccurred, this, [=](QTcpSocket::SocketError socketError) {
        ui.txt_state_log->appendPlainText(">> [Error] Enable Port 29999 Failed: " + m_robot_enable->errorString());
        });

    connect(m_robot_enable, &QTcpSocket::readyRead, this, [=]() {
        QElapsedTimer processTimer;
        processTimer.start();
        QByteArray data = m_robot_enable->readAll();

        if (m_client && m_client->isOpen()) {
            m_client->write(data);
        }

        double relayLatencyMs = processTimer.nsecsElapsed() / 1000000.0;
        updateProcessLatencyUI(3, relayLatencyMs);
        ui.txt_feedback_log->appendPlainText(">> Robot 29999 to Relay: " + data);
        });

    m_robot_enable->connectToHost(ip, enablePort);

    // 🚀 连接机械臂的 30004 端口
    m_robotStatusSocket->connectToHost(ip, 30004);
}

void RelayStation::on_btn_stop_clicked() {
    if (m_netTimer && m_netTimer->isActive()) m_netTimer->stop();
    m_isWaitingPONG = false;

    ui.txt_state_log->appendPlainText("===== Stopping Embodied AI Relay Station =====");

    if (m_server->isListening()) m_server->close();
    if (m_client && m_client->isOpen()) m_client->disconnectFromHost();

    if (m_statusServer->isListening()) m_statusServer->close();
    for (QTcpSocket* client : m_statusClients) {
        if (client->isOpen()) client->disconnectFromHost();
    }
    m_statusClients.clear();

    if (m_statusTimeoutTimer && m_statusTimeoutTimer->isActive()) {
        m_statusTimeoutTimer->stop();
    }
    if (ui.robot_30004_status) {
        ui.robot_30004_status->setText("closed");
    }

    if (m_robotStatusSocket && m_robotStatusSocket->isOpen()) {
        m_robotStatusSocket->disconnectFromHost();
        ui.txt_state_log->appendPlainText(">> Robot status port 30004 closed.");
    }

    if (m_robot_motion && m_robot_motion->isOpen()) {
        m_robot_motion->disconnectFromHost();
        ui.txt_state_log->appendPlainText(">> Robot motion port 30003 closed.");
    }
    if (m_robot_enable && m_robot_enable->isOpen()) {
        m_robot_enable->disconnectFromHost();
        ui.txt_state_log->appendPlainText(">> Robot enable port 29999 closed.");
    }

    ui.LED_Touch->setStyleSheet("QLabel { background-color: #A0A0A0; border-radius: 8px; border: 1px solid #888888; }");
    ui.LED_Robot->setStyleSheet("QLabel { background-color: #A0A0A0; border-radius: 8px; border: 1px solid #888888; }");

    ui.btn_start->setEnabled(true);
    ui.txt_state_log->appendPlainText("===== Relay Station Fully Closed =====");
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
    QPen penControl(QColor("#9D4EDD"));
    penControl.setWidth(2);
    ui.plot_relay_latency->graph(0)->setPen(penControl);

    ui.plot_relay_latency->addGraph();
    ui.plot_relay_latency->graph(1)->setName("Tactile");
    QPen penTactile(QColor("#9D4EDD"));
    penTactile.setWidth(2);
    ui.plot_relay_latency->graph(1)->setPen(penTactile);

    ui.plot_relay_latency->addGraph();
    ui.plot_relay_latency->graph(2)->setName("Video");
    QPen penVideo(QColor("#9D4EDD"));
    penVideo.setWidth(2);
    ui.plot_relay_latency->graph(2)->setPen(penVideo);

    ui.plot_relay_latency->addGraph();
    ui.plot_relay_latency->graph(3)->setName("Feedback (Robot -> Touch)");
    QPen penFeedback(QColor("#00D2FF"));
    penFeedback.setWidth(2);
    ui.plot_relay_latency->graph(3)->setPen(penFeedback);

    ui.plot_relay_latency->legend->setVisible(false);
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

    if (ui.plot_relay_latency->graphCount() > 2) {
        ui.plot_relay_latency->graph(1)->removeFromLegend();
        ui.plot_relay_latency->graph(2)->removeFromLegend();
    }
    ui.plot_relay_latency->replot();
}

void RelayStation::updateProcessLatencyUI(int channelIndex, double latencyMs) {
    m_latencyHistory[channelIndex].append(latencyMs);
    while (m_latencyHistory[channelIndex].size() > 20) {
        m_latencyHistory[channelIndex].removeFirst();
    }
    int currentX = 0;
    if (channelIndex == 0) currentX = ++m_packetCountControl;
    else if (channelIndex == 1) currentX = ++m_packetCountTactile;
    else if (channelIndex == 2) currentX = ++m_packetCountVideo;
    else if (channelIndex == 3) currentX = ++m_packetCountFeedback;

    ui.plot_relay_latency->graph(channelIndex)->addData(currentX, latencyMs);
    bool isCurrent = false;
    if (channelIndex == 0 && ui.chk_show_control->isChecked()) isCurrent = true;
    else if (channelIndex == 1 && ui.chk_show_tactile->isChecked()) isCurrent = true;
    else if (channelIndex == 2 && ui.chk_show_video->isChecked()) isCurrent = true;
    else if (channelIndex == 3 && ui.chk_show_control->isChecked()) isCurrent = true;

    if (isCurrent) {
        double sum = 0;
        for (double val : m_latencyHistory[channelIndex]) sum += val;
        double avg = (m_latencyHistory[channelIndex].size() > 0) ? (sum / m_latencyHistory[channelIndex].size()) : 0;
        if (channelIndex == 0) {
            ui.avg_relay_latency->setText(QString::number(avg, 'f', 3));
        }
        double maxInWindow = 0.15;
        for (double v : m_latencyHistory[channelIndex]) {
            if (v * 2.0 > maxInWindow) {
                maxInWindow = v * 2.0;
            }
        }
        ui.plot_relay_latency->yAxis->setRange(0, maxInWindow);
        if (currentX > 100) {
            ui.plot_relay_latency->xAxis->setRange(currentX - 100, currentX);
        }
        else {
            ui.plot_relay_latency->xAxis->setRange(0, 100);
        }
        ui.plot_relay_latency->replot();
    }
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

// =======================================================
// 🚀 核心：状态专线逻辑处理 (8889 & 30004 解包)
// =======================================================
void RelayStation::onNewStatusConnection() {
    while (m_statusServer->hasPendingConnections()) {
        QTcpSocket* client = m_statusServer->nextPendingConnection();
        m_statusClients.append(client);
        connect(client, &QTcpSocket::disconnected, this, &RelayStation::onStatusDisconnected);
        ui.txt_state_log->appendPlainText(QString(">> [Status Stream] Touch client joined 8889: %1").arg(client->peerAddress().toString()));
    }
}

void RelayStation::onStatusDisconnected() {
    QTcpSocket* client = qobject_cast<QTcpSocket*>(sender());
    if (client) {
        m_statusClients.removeOne(client);
        client->deleteLater();
        ui.txt_state_log->appendPlainText(">> [Status Stream] Touch client disconnected from 8889.");
    }
}

void RelayStation::onRobotStatusReadyRead() {
    m_statusBuffer.append(m_robotStatusSocket->readAll());

    while (m_statusBuffer.size() >= 1440) {
        QByteArray packet = m_statusBuffer.left(1440);
        m_statusBuffer.remove(0, 1440);

        DobotRealTimeData* data = reinterpret_cast<DobotRealTimeData*>(packet.data());
        double* pose = data->toolVectorActual;
        double* joint = data->qActual;

        QString twinStr = QString("TWIN:%1,%2,%3,%4,%5,%6|%7,%8,%9,%10,%11,%12\n")
            .arg(pose[0], 0, 'f', 2).arg(pose[1], 0, 'f', 2).arg(pose[2], 0, 'f', 2)
            .arg(pose[3], 0, 'f', 2).arg(pose[4], 0, 'f', 2).arg(pose[5], 0, 'f', 2)
            .arg(joint[0], 0, 'f', 2).arg(joint[1], 0, 'f', 2).arg(joint[2], 0, 'f', 2)
            .arg(joint[3], 0, 'f', 2).arg(joint[4], 0, 'f', 2).arg(joint[5], 0, 'f', 2);

        // 🚀 修改点：移除之前的 logCounter 逻辑，改为控制 QLineEdit 和重置定时器
        if (ui.robot_30004_status) {
            // 加一个判断，避免高频地重复设置相同文本导致 UI 无意义重绘
            if (ui.robot_30004_status->text() != "transmitting") {
                ui.robot_30004_status->setText("transmitting");
            }
        }

        // 收到数据，重置定时器。只要一直有数据来（每8ms一次），定时器就不会触发
        m_statusTimeoutTimer->start(500);

        for (QTcpSocket* client : m_statusClients) {
            if (client->state() == QAbstractSocket::ConnectedState) {
                client->write(twinStr.toUtf8());
            }
        }
    }
}