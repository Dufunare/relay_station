#include "relay_runtime.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <limits>

namespace relay {
namespace {

int replyControllerCode(const QByteArray& rawReply)
{
    const qsizetype comma = rawReply.indexOf(',');
    const QByteArray codeBytes = (comma < 0 ? rawReply : rawReply.left(comma)).trimmed();
    bool ok = false;
    const int code = codeBytes.toInt(&ok);
    return ok ? code : std::numeric_limits<int>::min();
}

bool isMotionCommand(const QString& name)
{
    static const QStringList motionNames = {
        QStringLiteral("MovL"), QStringLiteral("MovJ"), QStringLiteral("JointMovJ"),
        QStringLiteral("MovLIO"), QStringLiteral("MovJIO"), QStringLiteral("Arc"),
        QStringLiteral("Circle"), QStringLiteral("ServoP"), QStringLiteral("ServoJ")};
    for (const QString& candidate : motionNames) {
        if (name.compare(candidate, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

bool isGripperPlaceholderCommand(const QString& name)
{
    return name.startsWith(QStringLiteral("Gripper"), Qt::CaseInsensitive) ||
           name.startsWith(QStringLiteral("Jaw"), Qt::CaseInsensitive);
}

bool validateCompleteCommand(int targetPort, const QByteArray& command)
{
    LegacyCommandFramer parser;
    const QByteArray framed = QByteArray::number(targetPort) + '|' + command;
    const LegacyFeedResult result = parser.feed(framed);
    return !result.fatal && result.commands.size() == 1 && parser.bufferedBytes() == 0 &&
           result.commands.front().command == command.trimmed();
}

} // namespace

RelayRuntime::RelayRuntime(QObject* parent)
    : QObject(parent)
{
    m_dashboardSocket.setProxy(QNetworkProxy::NoProxy);
    m_motionSocket.setProxy(QNetworkProxy::NoProxy);
    m_feedbackSocket.setProxy(QNetworkProxy::NoProxy);
    m_dashboardSocket.setReadBufferSize(kMaxRobotReplyBufferBytes);
    m_motionSocket.setReadBufferSize(kMaxRobotReplyBufferBytes);
    m_feedbackSocket.setReadBufferSize(4 * kFeedbackFrameSize);

    connect(&m_legacyServer, &QTcpServer::newConnection, this, &RelayRuntime::acceptLegacyClient);
    connect(&m_v2Server, &QTcpServer::newConnection, this, &RelayRuntime::acceptV2Client);
    connect(&m_dashboardSocket, &QTcpSocket::readyRead, this, &RelayRuntime::readDashboardReplies);
    connect(&m_motionSocket, &QTcpSocket::readyRead, this, &RelayRuntime::readMotionReplies);
    connect(&m_feedbackSocket, &QTcpSocket::readyRead, this, &RelayRuntime::readFeedback);

    configureRobotSocket(&m_dashboardSocket, kDashboardPort);
    configureRobotSocket(&m_motionSocket, kMotionPort);
    configureRobotSocket(&m_feedbackSocket, kFeedbackPort);

    connect(&m_dashboardSocket, &QTcpSocket::bytesWritten, this, [this] {
        drainSocket(&m_dashboardSocket, &m_dashboardOutput);
    });
    connect(&m_motionSocket, &QTcpSocket::bytesWritten, this, [this] {
        drainSocket(&m_motionSocket, &m_motionOutput);
    });

    m_housekeepingTimer.setInterval(100);
    connect(&m_housekeepingTimer, &QTimer::timeout, this, &RelayRuntime::expireRequests);
    m_statusHeartbeatTimer.setInterval(200);
    connect(&m_statusHeartbeatTimer, &QTimer::timeout, this, &RelayRuntime::publishHeartbeat);
    m_stateStaleTimer.setSingleShot(true);
    m_stateStaleTimer.setInterval(501);
    connect(&m_stateStaleTimer, &QTimer::timeout, this, [this] {
        publishRelayStatus();
        emit connectionStatusChanged();
    });
    m_robotReconnectTimer.setInterval(1000);
    connect(&m_robotReconnectTimer, &QTimer::timeout, this, [this] {
        if (!m_running) return;
        const auto reconnect = [this](QTcpSocket* socket, quint16 port) {
            if (socket->state() == QAbstractSocket::UnconnectedState) {
                socket->connectToHost(m_robotIp, port);
            }
        };
        reconnect(&m_dashboardSocket, m_dashboardPort);
        reconnect(&m_motionSocket, m_motionPort);
        reconnect(&m_feedbackSocket, m_feedbackPort);
    });
}

RelayRuntime::~RelayRuntime()
{
    stop();
}

void RelayRuntime::configureRobotSocket(QTcpSocket* socket, int port)
{
    connect(socket, &QTcpSocket::connected, this, [this, port] {
        if (port == kFeedbackPort) {
            m_feedbackParser.connected();
            m_stateStore.invalidate(m_feedbackParser.connectionGeneration());
        }
        markPortChanged(port);
        emit logMessage(QStringLiteral(">> Robot port %1 connected.").arg(port));
        publishRelayStatus();
        emit connectionStatusChanged();
    });
    connect(socket, &QTcpSocket::disconnected, this, [this, port] {
        markPortChanged(port);
        QVector<PendingRequest> interrupted;
        if (port == kDashboardPort) {
            m_dashboardReplyFramer.reset();
            m_dashboardOutput.clear();
            interrupted = m_replyLedger.clearPort(port);
        } else if (port == kMotionPort) {
            m_motionReplyFramer.reset();
            m_motionOutput.clear();
            interrupted = m_replyLedger.clearPort(port);
        } else {
            m_feedbackParser.disconnected();
            m_stateStore.invalidate(m_feedbackParser.connectionGeneration());
            m_stateStaleTimer.stop();
        }
        for (const PendingRequest& request : interrupted) {
            if (request.origin == RequestOrigin::V2 && request.sessionId == m_v2SessionId) {
                queueV2Object(makeError(request.requestId, request.sessionId,
                                        QStringLiteral("robot_port_disconnected"),
                                        QStringLiteral("robot socket disconnected before its reply")));
            } else if (request.origin == RequestOrigin::Legacy8888 &&
                       request.connectionGeneration == m_legacyGeneration) {
                queueLegacyBytes(QStringLiteral("ERROR|robot_port_disconnected|%1;")
                                     .arg(port).toUtf8());
            }
        }
        if (!m_stopping) {
            emit logMessage(QStringLiteral(">> Robot port %1 disconnected; relay_ready=false.").arg(port));
        }
        publishRelayStatus();
        emit connectionStatusChanged();
    });
    connect(socket, &QTcpSocket::errorOccurred, this,
            [this, socket, port](QAbstractSocket::SocketError error) {
        Q_UNUSED(error);
        if (!m_stopping) {
            emit logMessage(QStringLiteral(">> [Error] Robot port %1: %2")
                                .arg(port)
                                .arg(socket->errorString()));
        }
    });
}

bool RelayRuntime::start(const QString& robotIp,
                         quint16 dashboardPort,
                         quint16 motionPort,
                         quint16 feedbackPort,
                         quint16 legacyPort,
                         quint16 v2Port)
{
    if (m_running) {
        return true;
    }
    m_stopping = false;

    // The Windows client attempts 8891 only once immediately after 8888 connects.
    // Therefore v2 must be listening before the legacy listener is exposed.
    if (!m_v2Server.listen(QHostAddress::Any, v2Port)) {
        emit logMessage(QStringLiteral(">> [Error] Reliable v2 port %1 listen failed: %2")
                            .arg(v2Port)
                            .arg(m_v2Server.errorString()));
        return false;
    }
    if (!m_legacyServer.listen(QHostAddress::Any, legacyPort)) {
        emit logMessage(QStringLiteral(">> [Error] Legacy port %1 listen failed: %2")
                            .arg(legacyPort)
                            .arg(m_legacyServer.errorString()));
        m_v2Server.close();
        return false;
    }

    m_running = true;
    m_robotIp = robotIp;
    m_dashboardPort = dashboardPort;
    m_motionPort = motionPort;
    m_feedbackPort = feedbackPort;
    m_stateStore.invalidate(m_feedbackParser.connectionGeneration());
    m_dashboardSocket.connectToHost(robotIp, dashboardPort);
    m_motionSocket.connectToHost(robotIp, motionPort);
    m_feedbackSocket.connectToHost(robotIp, feedbackPort);
    m_housekeepingTimer.start();
    m_statusHeartbeatTimer.start();
    m_robotReconnectTimer.start();
    emit logMessage(QStringLiteral(">> Reliable v2 server started on %1 before legacy server on %2.")
                        .arg(m_v2Server.serverPort())
                        .arg(m_legacyServer.serverPort()));
    emit logMessage(QStringLiteral(">> Reply timeout policy: ordinary=%1 ms, EnableRobot=%2 ms")
                        .arg(kRobotReplyTimeoutNs / 1000000LL)
                        .arg(kEnableRobotReplyTimeoutNs / 1000000LL));
    return true;
}

void RelayRuntime::stop()
{
    if (!m_running && !m_legacyServer.isListening() && !m_v2Server.isListening()) {
        return;
    }
    m_stopping = true;
    m_relayUiStateGate.reset();
    ++m_diagnostics.stopCount;
    m_legacyServer.close();
    m_v2Server.close();
    m_housekeepingTimer.stop();
    m_statusHeartbeatTimer.stop();
    m_stateStaleTimer.stop();
    m_robotReconnectTimer.stop();
    clearSessionData();
    closeLegacyClient();
    closeV2Client();
    m_feedbackParser.disconnected();
    m_stateStore.invalidate(m_feedbackParser.connectionGeneration());

    for (QTcpSocket* socket : {&m_feedbackSocket, &m_motionSocket, &m_dashboardSocket}) {
        socket->abort();
    }
    m_running = false;
    m_lastReady = false;
    m_stopping = false;
    emit sessionReset();
    emit connectionStatusChanged();
}

bool RelayRuntime::portConnected(int port) const
{
    const QTcpSocket* socket = nullptr;
    if (port == kDashboardPort) socket = &m_dashboardSocket;
    else if (port == kMotionPort) socket = &m_motionSocket;
    else if (port == kFeedbackPort) socket = &m_feedbackSocket;
    return socket && socket->state() == QAbstractSocket::ConnectedState;
}

void RelayRuntime::setStreamDiagnostics(const StreamDiagnostics& diagnostics)
{
    m_streamDiagnostics = diagnostics;
}

void RelayRuntime::acceptLegacyClient()
{
    while (m_legacyServer.hasPendingConnections()) {
        replaceLegacyClient(m_legacyServer.nextPendingConnection());
    }
}

void RelayRuntime::acceptV2Client()
{
    while (m_v2Server.hasPendingConnections()) {
        replaceV2Client(m_v2Server.nextPendingConnection());
    }
}

void RelayRuntime::replaceLegacyClient(QTcpSocket* socket)
{
    if (m_legacyClient) {
        ++m_diagnostics.sessionSwitches;
        closeLegacyClient();
        closeV2Client();
        emit sessionReset();
    }
    ++m_legacyGeneration;
    ++m_diagnostics.legacyConnections;
    m_legacyFramer.reset();
    m_legacyOutput.clear();
    m_legacyClient = socket;
    socket->setParent(this);
    socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    socket->setReadBufferSize(kMaxLegacyBufferBytes);
    connect(socket, &QTcpSocket::readyRead, this, &RelayRuntime::readLegacyClient);
    connect(socket, &QTcpSocket::bytesWritten, this, [this] {
        if (m_legacyClient) drainSocket(m_legacyClient, &m_legacyOutput);
    });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
        if (m_legacyClient != socket) return;
        ++m_diagnostics.legacyDisconnects;
        m_replyLedger.clearConnection(RequestOrigin::Legacy8888, m_legacyGeneration);
        m_legacyFramer.reset();
        m_legacyOutput.clear();
        m_legacyClient.clear();
        socket->deleteLater();
        closeV2Client();
        emit sessionReset();
        emit legacyClientChanged(false);
        emit logMessage(QStringLiteral(">> Legacy 8888 client disconnected; session data cleared."));
    });
    emit legacyClientChanged(true);
    emit logMessage(QStringLiteral(">> Legacy 8888 client connected (generation %1).")
                        .arg(m_legacyGeneration));
}

void RelayRuntime::replaceV2Client(QTcpSocket* socket)
{
    if (m_v2Client) {
        ++m_diagnostics.sessionSwitches;
        closeV2Client();
        emit sessionReset();
    }
    ++m_diagnostics.v2Connections;
    m_v2SessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_v2Handshaken = false;
    m_pushRobotState = true;
    m_pushRelayStatus = true;
    m_v2Parser.reset();
    m_v2Output.clear();
    m_latestV2StateFrame.reset();
    m_latestV2StatusFrame.reset();
    m_v2RobotGate.reset();
    m_haveV2ReportedRobotState=false;
    m_actionGate.beginSession(m_v2SessionId);
    m_v2Client = socket;
    socket->setParent(this);
    socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    socket->setReadBufferSize(kMaxV2BodyBytes + 4);
    connect(socket, &QTcpSocket::readyRead, this, &RelayRuntime::readV2Client);
    connect(socket, &QTcpSocket::bytesWritten, this, [this] { drainV2Output(); });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
        if (m_v2Client != socket) return;
        ++m_diagnostics.v2Disconnects;
        const QString oldSession = m_v2SessionId;
        m_v2Client.clear();
        socket->deleteLater();
        m_replyLedger.clearSession(oldSession);
        invalidateV2Session();
        emit sessionReset();
        emit logMessage(QStringLiteral(">> Reliable 8891 client disconnected; old session invalidated."));
    });
}

void RelayRuntime::closeLegacyClient()
{
    if (!m_legacyClient) return;
    QTcpSocket* socket = m_legacyClient;
    m_legacyClient.clear();
    m_replyLedger.clearConnection(RequestOrigin::Legacy8888, m_legacyGeneration);
    m_legacyFramer.reset();
    m_legacyOutput.clear();
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
}

void RelayRuntime::closeV2Client()
{
    if (!m_v2Client) {
        invalidateV2Session();
        return;
    }
    QTcpSocket* socket = m_v2Client;
    m_v2Client.clear();
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
    invalidateV2Session();
}

void RelayRuntime::invalidateV2Session()
{
    m_v2RobotGate.reset();
    m_haveV2ReportedRobotState=false;
    if (!m_v2SessionId.isEmpty()) {
        m_replyLedger.clearSession(m_v2SessionId);
    }
    m_v2SessionId.clear();
    m_v2Handshaken = false;
    m_v2Parser.reset();
    m_v2Output.clear();
    m_latestV2StateFrame.reset();
    m_latestV2StatusFrame.reset();
    m_actionGate.clear();
}

void RelayRuntime::clearSessionData()
{
    m_replyLedger.clearAll();
    m_legacyFramer.reset();
    m_v2Parser.reset();
    m_dashboardReplyFramer.reset();
    m_motionReplyFramer.reset();
    m_legacyOutput.clear();
    m_v2Output.clear();
    m_dashboardOutput.clear();
    m_motionOutput.clear();
    m_latestV2StateFrame.reset();
    m_latestV2StatusFrame.reset();
    m_actionGate.clear();
}

void RelayRuntime::readLegacyClient()
{
    if (!m_legacyClient) return;
    const qint64 receivedNs = steadyNowNs();
    const QByteArray bytes = m_legacyClient->readAll();
    const qsizetype before = m_legacyFramer.bufferedBytes();
    const LegacyFeedResult result = m_legacyFramer.feed(bytes, receivedNs);
    if (before > 0 || (result.commands.isEmpty() && !bytes.isEmpty())) {
        ++m_diagnostics.legacyPartialReads;
    }
    if (result.commands.size() > 1) {
        ++m_diagnostics.legacyCoalescedReads;
    }
    m_diagnostics.legacyFrames += static_cast<quint64>(result.commands.size());
    for (const LegacyCommand& command : result.commands) {
        routeLegacyCommand(command, receivedNs);
    }
    if (result.fatal) {
        ++m_diagnostics.legacyParseFailures;
        const ProtocolError error = result.errors.front();
        queueLegacyBytes(QStringLiteral("ERROR|%1|%2;")
                             .arg(error.code, error.detail)
                             .toUtf8());
        if (m_legacyClient) m_legacyClient->disconnectFromHost();
    }
}

void RelayRuntime::routeLegacyCommand(const LegacyCommand& command, qint64 receivedNs)
{
    const QString name = commandName(command.command);
    if (isGripperPlaceholderCommand(name)) {
        const ProtocolError error = m_gripper.rejectUnavailableCommand();
        queueLegacyBytes(QStringLiteral("ERROR|%1|%2;").arg(error.code, error.detail).toUtf8());
        return;
    }
    // Restored the original 8888 -> 30003 ServoP path for the
    // manually armed Touch teleop controller. It is a non-blocking stream,
    // not an ordinary queued motion/reply. Do NOT enable v2 motion here.
    if (name.compare(QStringLiteral("ServoP"), Qt::CaseInsensitive) == 0 &&
        command.targetPort != kMotionPort) {
        queueLegacyBytes("ERROR|servop_requires_30003|ServoP must use motion port;");
        return;
    }
    QTcpSocket* socket = command.targetPort == kDashboardPort ? &m_dashboardSocket : &m_motionSocket;
    ShortWriteBuffer* output = command.targetPort == kDashboardPort ? &m_dashboardOutput : &m_motionOutput;
    if (socket->state() != QAbstractSocket::ConnectedState) {
        queueLegacyBytes(QStringLiteral("ERROR|robot_port_disconnected|%1;")
                             .arg(command.targetPort)
                             .toUtf8());
        return;
    }
    if (!routeToRobot(command.targetPort, command.command, output)) {
        queueLegacyBytes(QStringLiteral("ERROR|robot_write_queue_full|%1;")
                             .arg(command.targetPort)
                             .toUtf8());
        return;
    }
    if (commandExpectsControllerReply(command.command)) {
        PendingRequest request;
        request.origin = RequestOrigin::Legacy8888;
        request.connectionGeneration = m_legacyGeneration;
        request.command = command.command;
        request.relayReceiveSteadyNs = receivedNs;
        request.deadlineSteadyNs = receivedNs + robotReplyTimeoutNs(command.command);
        m_replyLedger.enqueue(command.targetPort, request);
    }
    const double elapsedMs = static_cast<double>(steadyNowNs() - receivedNs) / 1.0e6;
    emit commandRouted(command.targetPort, command.command, elapsedMs);
}

void RelayRuntime::readV2Client()
{
    if (!m_v2Client) return;
    const qint64 receivedNs = steadyNowNs();
    const V2FeedResult result = m_v2Parser.feed(m_v2Client->readAll());
    m_diagnostics.v2ReceivedFrames += static_cast<quint64>(result.messages.size());
    if (result.fatal) {
        const ProtocolError error = result.errors.front();
        protocolCloseV2(QStringLiteral("0"), error.code, error.detail);
        return;
    }
    for (const QJsonObject& object : result.messages) {
        handleV2Message(object, receivedNs);
        if (!m_v2Client) break;
    }
}

void RelayRuntime::handleV2Message(const QJsonObject& object, qint64 receivedNs)
{
    const QString requestId = object.value(QStringLiteral("request_id")).toString();
    const QString type = object.value(QStringLiteral("type")).toString();
    const QString session = object.value(QStringLiteral("session_id")).toString();
    if (!m_v2Handshaken) {
        if (type != QStringLiteral("hello")) {
            protocolCloseV2(requestId, QStringLiteral("hello_required"),
                            QStringLiteral("hello must be the first v2 message"));
            return;
        }
        handleHello(object);
        return;
    }
    if (requestId == QStringLiteral("0")) {
        protocolCloseV2(requestId, QStringLiteral("invalid_request_id"),
                        QStringLiteral("client requests must not use request_id 0"));
        return;
    }
    if (session.isEmpty() || session != m_v2SessionId) {
        protocolCloseV2(requestId, QStringLiteral("session_mismatch"),
                        QStringLiteral("message session is empty, stale, or belongs to another connection"));
        return;
    }
    if (type == QStringLiteral("command")) {
        handleV2Command(object, receivedNs);
    } else if (type == QStringLiteral("stream_control")) {
        handleStreamControl(object);
    } else {
        protocolCloseV2(requestId, QStringLiteral("invalid_client_type"),
                        QStringLiteral("client may send only command or stream_control after hello"));
    }
}

void RelayRuntime::handleHello(const QJsonObject& object)
{
    const QString requestId = object.value(QStringLiteral("request_id")).toString();
    if (requestId == QStringLiteral("0") || !object.value(QStringLiteral("session_id")).toString().isEmpty() ||
        object.value(QStringLiteral("client")).toString() != QStringLiteral("touchvla_windows") ||
        !object.value(QStringLiteral("capabilities")).isArray()) {
        protocolCloseV2(requestId, QStringLiteral("invalid_hello"),
                        QStringLiteral("hello requires Windows client, nonzero request, empty session and capabilities"));
        return;
    }
    const QJsonArray capabilities = object.value(QStringLiteral("capabilities")).toArray();
    m_pushRobotState = capabilities.contains(QStringLiteral("robot_state"));
    m_pushRelayStatus = capabilities.contains(QStringLiteral("relay_status"));
    m_v2Handshaken = true;
    ++m_diagnostics.v2Handshakes;
    queueV2Object({{QStringLiteral("v"), 2},
                   {QStringLiteral("type"), QStringLiteral("hello_ack")},
                   {QStringLiteral("request_id"), requestId},
                   {QStringLiteral("session_id"), m_v2SessionId}});
    publishRelayStatus();
    const RobotStateSnapshot state = m_stateStore.latest();
    if(state.valid) {
        publishRobotState(state);
        m_lastV2ReportedRobotState=state;
        m_haveV2ReportedRobotState=true;
        m_v2RobotGate.allow(steadyNowNs(),true);
    }
}

void RelayRuntime::handleV2Command(const QJsonObject& object, qint64 receivedNs)
{
    const QString requestId = object.value(QStringLiteral("request_id")).toString();
    if (m_replyLedger.hasV2Request(m_v2SessionId, requestId)) {
        queueV2Object(makeError(requestId, m_v2SessionId, QStringLiteral("duplicate_request_id"),
                                QStringLiteral("request_id is already awaiting a controller reply")));
        return;
    }
    const QJsonValue portValue = object.value(QStringLiteral("target_port"));
    const QJsonValue commandValue = object.value(QStringLiteral("command"));
    if (!portValue.isDouble() || !commandValue.isString()) {
        protocolCloseV2(requestId, QStringLiteral("invalid_command"),
                        QStringLiteral("command requires integer target_port and string command"));
        return;
    }
    const double targetPortNumber = portValue.toDouble();
    if (!std::isfinite(targetPortNumber) || targetPortNumber != std::floor(targetPortNumber)) {
        protocolCloseV2(requestId, QStringLiteral("invalid_command"),
                        QStringLiteral("target_port must be an integer"));
        return;
    }
    const int targetPort = portValue.toInt();
    const QByteArray command = commandValue.toString().toUtf8();
    if ((targetPort != kDashboardPort && targetPort != kMotionPort) || command.isEmpty() ||
        command.size() > kMaxLegacyCommandBytes || !validateCompleteCommand(targetPort, command)) {
        protocolCloseV2(requestId, QStringLiteral("invalid_command"),
                        QStringLiteral("target must be 29999/30003 and command must be one bounded complete call"));
        return;
    }
    if (object.contains(QStringLiteral("client_send_unix_ns")) &&
        (!object.value(QStringLiteral("client_send_unix_ns")).isString() ||
         !isDecimalId(object.value(QStringLiteral("client_send_unix_ns")).toString()))) {
        protocolCloseV2(requestId, QStringLiteral("invalid_client_timestamp"),
                        QStringLiteral("client_send_unix_ns must be an unsigned decimal string"));
        return;
    }

    const QString name = commandName(command);
    if (isGripperPlaceholderCommand(name)) {
        const ProtocolError error = m_gripper.rejectUnavailableCommand();
        queueV2Object(makeError(requestId, m_v2SessionId, error.code, error.detail));
        return;
    }
    if (targetPort == kMotionPort && isMotionCommand(name)) {
        if (object.contains(QStringLiteral("control_generation")) ||
            object.contains(QStringLiteral("action_seq")) || object.contains(QStringLiteral("ttl_ms"))) {
            if (!object.value(QStringLiteral("control_generation")).isString() ||
                !object.value(QStringLiteral("action_seq")).isString() ||
                !object.value(QStringLiteral("ttl_ms")).isDouble()) {
                queueV2Object(makeError(requestId, m_v2SessionId, QStringLiteral("invalid_action_fields"),
                                        QStringLiteral("generation/action_seq are decimal strings and ttl_ms is numeric")));
                return;
            }
            const double ttlMs = object.value(QStringLiteral("ttl_ms")).toDouble();
            if (!std::isfinite(ttlMs) || ttlMs <= 0.0 || ttlMs > 60000.0) {
                queueV2Object(makeError(requestId, m_v2SessionId, QStringLiteral("invalid_ttl"),
                                        QStringLiteral("ttl_ms must be finite and in (0, 60000]")));
                return;
            }
            const ActionDecision decision = m_actionGate.offer(
                {m_v2SessionId,
                 object.value(QStringLiteral("control_generation")).toString(),
                 object.value(QStringLiteral("action_seq")).toString(),
                 receivedNs,
                 static_cast<qint64>(ttlMs * 1.0e6),
                 command});
            if (!decision.accepted) {
                queueV2Object(makeError(requestId, m_v2SessionId, decision.errorCode,
                                        QStringLiteral("continuous target rejected before robot write")));
                return;
            }
        }
        if (!kV2MotionProductionEnabled) {
            m_actionGate.beginSession(m_v2SessionId);
            queueV2Object(makeError(requestId, m_v2SessionId,
                                    QStringLiteral("v2_motion_path_disabled"),
                                    QStringLiteral("v2 motion forwarding is offline-prepared and production-disabled")));
            return;
        }
    }

    QTcpSocket* socket = targetPort == kDashboardPort ? &m_dashboardSocket : &m_motionSocket;
    ShortWriteBuffer* output = targetPort == kDashboardPort ? &m_dashboardOutput : &m_motionOutput;
    if (socket->state() != QAbstractSocket::ConnectedState) {
        queueV2Object(makeError(requestId, m_v2SessionId, QStringLiteral("robot_port_disconnected"),
                                QStringLiteral("target robot socket is not connected")));
        return;
    }
    if (!routeToRobot(targetPort, command, output)) {
        queueV2Object(makeError(requestId, m_v2SessionId, QStringLiteral("robot_write_queue_full"),
                                QStringLiteral("robot socket output is bounded and currently full")));
        return;
    }
    if (!commandExpectsControllerReply(command)) {
        queueV2Object({{QStringLiteral("v"), 2},
                       {QStringLiteral("type"), QStringLiteral("reply")},
                       {QStringLiteral("request_id"), requestId},
                       {QStringLiteral("session_id"), m_v2SessionId},
                       {QStringLiteral("target_port"), targetPort},
                       {QStringLiteral("command_name"), name},
                       {QStringLiteral("write_ack"), true},
                       {QStringLiteral("semantics"), QStringLiteral("bytes_accepted_by_relay_socket")}});
        return;
    }
    PendingRequest request;
    request.origin = RequestOrigin::V2;
    request.sessionId = m_v2SessionId;
    request.requestId = requestId;
    request.command = command;
    request.relayReceiveSteadyNs = receivedNs;
    request.deadlineSteadyNs = receivedNs + robotReplyTimeoutNs(command);
    m_replyLedger.enqueue(targetPort, request);
    emit commandRouted(targetPort, command, static_cast<double>(steadyNowNs() - receivedNs) / 1.0e6);
}

void RelayRuntime::handleStreamControl(const QJsonObject& object)
{
    const QString requestId = object.value(QStringLiteral("request_id")).toString();
    if (object.contains(QStringLiteral("robot_state"))) {
        if (!object.value(QStringLiteral("robot_state")).isBool()) {
            protocolCloseV2(requestId, QStringLiteral("invalid_stream_control"),
                            QStringLiteral("robot_state must be boolean"));
            return;
        }
        m_pushRobotState = object.value(QStringLiteral("robot_state")).toBool();
    }
    if (object.contains(QStringLiteral("relay_status"))) {
        if (!object.value(QStringLiteral("relay_status")).isBool()) {
            protocolCloseV2(requestId, QStringLiteral("invalid_stream_control"),
                            QStringLiteral("relay_status must be boolean"));
            return;
        }
        m_pushRelayStatus = object.value(QStringLiteral("relay_status")).toBool();
    }
    queueV2Object({{QStringLiteral("v"), 2},
                   {QStringLiteral("type"), QStringLiteral("reply")},
                   {QStringLiteral("request_id"), requestId},
                   {QStringLiteral("session_id"), m_v2SessionId},
                   {QStringLiteral("stream_control_applied"), true}});
}

void RelayRuntime::readDashboardReplies()
{
    handleRobotReplies(kDashboardPort, &m_dashboardReplyFramer, m_dashboardSocket.readAll());
}

void RelayRuntime::readMotionReplies()
{
    handleRobotReplies(kMotionPort, &m_motionReplyFramer, m_motionSocket.readAll());
}

void RelayRuntime::handleRobotReplies(int targetPort,
                                      RobotReplyFramer* framer,
                                      const QByteArray& bytes)
{
    const qint64 receivedNs = steadyNowNs();
    const ReplyFeedResult result = framer->feed(bytes);
    if (result.fatal) {
        emit logMessage(QStringLiteral(">> [Error] Robot %1 reply framing failed: %2")
                            .arg(targetPort)
                            .arg(result.errors.front().code));
        return;
    }
    for (const QByteArray& reply : result.replies) {
        if (replyControllerCode(reply) == std::numeric_limits<int>::min()) {
            emit logMessage(QStringLiteral(">> Robot %1 invalid/orphan reply discarded: %2")
                                .arg(targetPort)
                                .arg(QString::fromUtf8(reply.left(160))));
            continue;
        }
        const ReplyMatch match = m_replyLedger.match(targetPort, reply, receivedNs);
        if (match.orphan) {
            emit logMessage(QStringLiteral(">> Robot %1 orphan_reply discarded: %2")
                                .arg(targetPort)
                                .arg(QString::fromUtf8(reply.left(160))));
        } else {
            deliverReply(targetPort, match);
        }
    }
}

void RelayRuntime::deliverReply(int targetPort, const ReplyMatch& match)
{
    if (!match.request.has_value()) return;
    const PendingRequest& request = *match.request;
    if (request.origin == RequestOrigin::Legacy8888) {
        if (request.connectionGeneration == m_legacyGeneration && m_legacyClient) {
            queueLegacyBytes(match.rawReply);
        }
    } else if (m_v2Client && m_v2Handshaken && request.sessionId == m_v2SessionId) {
        queueV2Object({{QStringLiteral("v"), 2},
                       {QStringLiteral("type"), QStringLiteral("reply")},
                       {QStringLiteral("request_id"), request.requestId},
                       {QStringLiteral("session_id"), request.sessionId},
                       {QStringLiteral("target_port"), targetPort},
                       {QStringLiteral("command_name"), commandName(request.command)},
                       {QStringLiteral("controller_code"), replyControllerCode(match.rawReply)},
                       {QStringLiteral("raw_reply"), QString::fromUtf8(match.rawReply)},
                       {QStringLiteral("relay_receive_steady_ns"),
                        QString::number(request.relayReceiveSteadyNs)},
                       {QStringLiteral("robot_reply_receive_steady_ns"),
                        QString::number(match.robotReplyReceiveSteadyNs)}});
    }
    emit robotReplyObserved(targetPort, match.rawReply,
                            static_cast<double>(steadyNowNs() - match.robotReplyReceiveSteadyNs) / 1.0e6);
}

bool RelayRuntime::routeToRobot(int targetPort,
                                const QByteArray& command,
                                ShortWriteBuffer* writeBuffer)
{
    QTcpSocket* socket = targetPort == kDashboardPort ? &m_dashboardSocket : &m_motionSocket;
    if (!socket || socket->state() != QAbstractSocket::ConnectedState ||
        socket->bytesToWrite() + writeBuffer->pendingBytes() + command.size() > kMaxSocketOutputBytes ||
        !writeBuffer->enqueue(command)) {
        ++m_diagnostics.robotWriteRejects;
        return false;
    }
    drainSocket(socket, writeBuffer);
    return true;
}

void RelayRuntime::drainSocket(QTcpSocket* socket, ShortWriteBuffer* writeBuffer)
{
    if (!socket || socket->state() != QAbstractSocket::ConnectedState) return;
    writeBuffer->drain([socket](const char* data, qint64 size) { return socket->write(data, size); });
}

bool RelayRuntime::queueLegacyBytes(const QByteArray& bytes)
{
    if (!m_legacyClient ||
        m_legacyClient->bytesToWrite() + m_legacyOutput.pendingBytes() + bytes.size() >
            kMaxSocketOutputBytes ||
        !m_legacyOutput.enqueue(bytes)) {
        return false;
    }
    drainSocket(m_legacyClient, &m_legacyOutput);
    return true;
}

bool RelayRuntime::queueV2Object(const QJsonObject& object, bool stateUpdate, bool statusUpdate)
{
    if (!m_v2Client) return false;
    const QByteArray frame = encodeV2Frame(object);
    if (frame.isEmpty()) return false;
    const qsizetype outstanding = m_v2Client->bytesToWrite() + m_v2Output.pendingBytes();
    if ((stateUpdate || statusUpdate) && outstanding >= kSlowV2ThresholdBytes) {
        std::optional<QByteArray>& latest = stateUpdate ? m_latestV2StateFrame : m_latestV2StatusFrame;
        if (latest.has_value()) ++m_diagnostics.v2StateOverwrites;
        latest = frame;
        return true;
    }
    if (outstanding + frame.size() > kMaxSocketOutputBytes || !m_v2Output.enqueue(frame)) {
        ++m_diagnostics.v2SendRejects;
        return false;
    }
    ++m_diagnostics.v2SentFrames;
    drainV2Output();
    return true;
}

void RelayRuntime::drainV2Output()
{
    if (!m_v2Client || m_v2Client->state() != QAbstractSocket::ConnectedState) return;
    drainSocket(m_v2Client, &m_v2Output);
    if (m_v2Client->bytesToWrite() + m_v2Output.pendingBytes() < kSlowV2ThresholdBytes / 2) {
        for (std::optional<QByteArray>* latest : {&m_latestV2StatusFrame, &m_latestV2StateFrame}) {
            if (latest->has_value() &&
                m_v2Output.enqueue(**latest)) {
                latest->reset();
                ++m_diagnostics.v2SentFrames;
                drainSocket(m_v2Client, &m_v2Output);
            }
        }
    }
}

void RelayRuntime::readFeedback()
{
    const FeedbackFeedResult result =
        m_feedbackParser.feed(m_feedbackSocket.readAll(), steadyNowNs(), unixNowNs());
    for (const ProtocolError& error : result.errors) {
        emitFeedbackErrorLimited(error);
    }
    for (const RobotStateSnapshot& state : result.states) {
        // Preserve EVERY feedback frame in the internal state store.
        // Limit ordinary 8891 pushes to 50Hz to keep status heartbeats
        // responsive under 30004 bursts or camera processing load.
        m_stateStore.publish(state);
        const bool urgent=!m_haveV2ReportedRobotState ||
            state.errorStatus!=m_lastV2ReportedRobotState.errorStatus ||
            state.enableStatus!=m_lastV2ReportedRobotState.enableStatus ||
            state.dragStatus!=m_lastV2ReportedRobotState.dragStatus ||
            state.jogStatus!=m_lastV2ReportedRobotState.jogStatus ||
            state.pauseCmdFlag!=m_lastV2ReportedRobotState.pauseCmdFlag ||
            state.robotMode!=m_lastV2ReportedRobotState.robotMode ||
            state.runningStatus!=m_lastV2ReportedRobotState.runningStatus ||
            state.user!=m_lastV2ReportedRobotState.user ||
            state.tool!=m_lastV2ReportedRobotState.tool;
        if(m_v2Client && m_v2Handshaken && m_pushRobotState) {
            if(m_v2RobotGate.allow(steadyNowNs(),urgent)) {
                publishRobotState(state);
                m_lastV2ReportedRobotState=state;
                m_haveV2ReportedRobotState=true;
            } else {
                ++m_diagnostics.v2StateRateLimited;
            }
        }
    }
    if (!result.states.isEmpty()) {
        m_stateStaleTimer.start();
        if(m_relayUiStateGate.allow(steadyNowNs()))
            emit robotStateAvailable();
        const bool ready = relayReady(relayStatusInput());
        if (ready != m_lastReady) {
            publishRelayStatus();
            emit connectionStatusChanged();
        }
        m_lastReady = ready;
    }
}

void RelayRuntime::publishRobotState(const RobotStateSnapshot& state)
{
    if (m_v2Client && m_v2Handshaken && m_pushRobotState) {
        queueV2Object(robotStateJson(state, m_v2SessionId), true, false);
    }
}

RelayStatusInput RelayRuntime::relayStatusInput() const
{
    RelayStatusInput input;
    input.port29999Connected = portConnected(kDashboardPort);
    input.port30003Connected = portConnected(kMotionPort);
    input.port30004Connected = portConnected(kFeedbackPort);
    input.nowSteadyNs = steadyNowNs();
    input.state = m_stateStore.latest();
    return input;
}

QJsonObject RelayRuntime::enrichedRelayStatus() const
{
    QJsonObject status = relayStatusJson(relayStatusInput(), m_v2SessionId);
    const FeedbackStats feedback = m_feedbackParser.stats();
    status.insert(QStringLiteral("feedback_bytes"), QString::number(feedback.bytesReceived));
    status.insert(QStringLiteral("feedback_valid_frames"), QString::number(feedback.validFrames));
    status.insert(QStringLiteral("feedback_invalid_frames"), QString::number(feedback.invalidFrames));
    status.insert(QStringLiteral("feedback_message_size_errors"),
                  QString::number(feedback.messageSizeErrors));
    status.insert(QStringLiteral("feedback_test_value_errors"),
                  QString::number(feedback.testValueErrors));
    status.insert(QStringLiteral("feedback_non_finite_errors"),
                  QString::number(feedback.nonFiniteErrors));
    status.insert(QStringLiteral("feedback_invalid_flag_errors"),
                  QString::number(feedback.invalidFlagErrors));
    status.insert(QStringLiteral("feedback_misaligned_bytes"), QString::number(feedback.misalignedBytes));
    status.insert(QStringLiteral("feedback_resync_count"), QString::number(feedback.resyncCount));
    status.insert(QStringLiteral("feedback_buffer_drops"), QString::number(feedback.bufferDrops));
    status.insert(QStringLiteral("orphan_29999"), QString::number(m_replyLedger.orphanCount(kDashboardPort)));
    status.insert(QStringLiteral("orphan_30003"), QString::number(m_replyLedger.orphanCount(kMotionPort)));
    status.insert(QStringLiteral("reply_timeout_29999"),
                  QString::number(m_replyLedger.timeoutCount(kDashboardPort)));
    status.insert(QStringLiteral("reply_timeout_30003"),
                  QString::number(m_replyLedger.timeoutCount(kMotionPort)));
    status.insert(QStringLiteral("legacy_frames"), QString::number(m_diagnostics.legacyFrames));
    status.insert(QStringLiteral("legacy_client_connected"), !m_legacyClient.isNull());
    status.insert(QStringLiteral("legacy_connections"),
                  QString::number(m_diagnostics.legacyConnections));
    status.insert(QStringLiteral("legacy_disconnects"),
                  QString::number(m_diagnostics.legacyDisconnects));
    status.insert(QStringLiteral("legacy_partial_reads"),
                  QString::number(m_diagnostics.legacyPartialReads));
    status.insert(QStringLiteral("legacy_coalesced_reads"),
                  QString::number(m_diagnostics.legacyCoalescedReads));
    status.insert(QStringLiteral("legacy_parse_failures"),
                  QString::number(m_diagnostics.legacyParseFailures));
    status.insert(QStringLiteral("v2_handshakes"), QString::number(m_diagnostics.v2Handshakes));
    status.insert(QStringLiteral("v2_client_connected"), !m_v2Client.isNull());
    status.insert(QStringLiteral("v2_handshaken"), m_v2Handshaken);
    status.insert(QStringLiteral("v2_connections"), QString::number(m_diagnostics.v2Connections));
    status.insert(QStringLiteral("v2_disconnects"), QString::number(m_diagnostics.v2Disconnects));
    status.insert(QStringLiteral("v2_protocol_rejects"),
                  QString::number(m_diagnostics.v2ProtocolRejects));
    status.insert(QStringLiteral("v2_received_frames"),
                  QString::number(m_diagnostics.v2ReceivedFrames));
    status.insert(QStringLiteral("v2_sent_frames"), QString::number(m_diagnostics.v2SentFrames));
    status.insert(QStringLiteral("v2_state_rate_limited"), QString::number(m_diagnostics.v2StateRateLimited));
    status.insert(QStringLiteral("v2_state_overwrites"),
                  QString::number(m_diagnostics.v2StateOverwrites));
    status.insert(QStringLiteral("v2_send_rejects"),
                  QString::number(m_diagnostics.v2SendRejects));
    status.insert(QStringLiteral("robot_write_rejects"),
                  QString::number(m_diagnostics.robotWriteRejects));
    status.insert(QStringLiteral("stop_count"), QString::number(m_diagnostics.stopCount));
    status.insert(QStringLiteral("session_switches"),
                  QString::number(m_diagnostics.sessionSwitches));
    status.insert(QStringLiteral("action_expired"), QString::number(m_actionGate.expiredCount()));
    status.insert(QStringLiteral("action_replacements"),
                  QString::number(m_actionGate.replacementCount()));
    status.insert(QStringLiteral("port_29999_changed_steady_ns"),
                  QString::number(m_diagnostics.port29999ChangedSteadyNs));
    status.insert(QStringLiteral("port_30003_changed_steady_ns"),
                  QString::number(m_diagnostics.port30003ChangedSteadyNs));
    status.insert(QStringLiteral("port_30004_changed_steady_ns"),
                  QString::number(m_diagnostics.port30004ChangedSteadyNs));
    status.insert(QStringLiteral("twin_subscribed"), m_streamDiagnostics.twinSubscribed);
    status.insert(QStringLiteral("force_subscribed"), m_streamDiagnostics.forceSubscribed);
    status.insert(QStringLiteral("video_subscribed"), m_streamDiagnostics.videoSubscribed);
    status.insert(QStringLiteral("twin_sent"), QString::number(m_streamDiagnostics.twinSent));
    status.insert(QStringLiteral("force_sent"), QString::number(m_streamDiagnostics.forceSent));
    status.insert(QStringLiteral("video_sent"), QString::number(m_streamDiagnostics.videoSent));
    status.insert(QStringLiteral("camera_frame_seq"), QString::number(m_streamDiagnostics.cameraFrameSeq));
    status.insert(QStringLiteral("camera_last_frame_age_ms"),
                  m_streamDiagnostics.cameraLastCaptureSteadyNs > 0
                      ? static_cast<double>(std::max<qint64>(0, steadyNowNs() -
                                                                  m_streamDiagnostics.cameraLastCaptureSteadyNs)) /
                            1.0e6
                      : 1.0e12);
    status.insert(QStringLiteral("camera_last_bytes"),
                  static_cast<qint64>(m_streamDiagnostics.cameraLastBytes));
    status.insert(QStringLiteral("camera_capture_steady_ns"),
                  QString::number(m_streamDiagnostics.cameraLastCaptureSteadyNs));
    status.insert(QStringLiteral("camera_encoded_steady_ns"),
                  QString::number(m_streamDiagnostics.cameraLastEncodedSteadyNs));
    status.insert(QStringLiteral("camera_send_steady_ns"),
                  QString::number(m_streamDiagnostics.cameraLastSendSteadyNs));
    status.insert(QStringLiteral("camera_width"), m_streamDiagnostics.cameraLastWidth);
    status.insert(QStringLiteral("camera_height"), m_streamDiagnostics.cameraLastHeight);
    status.insert(QStringLiteral("camera_encoding"), m_streamDiagnostics.cameraLastEncoding);
    status.insert(QStringLiteral("camera_capture_failures"),
                  QString::number(m_streamDiagnostics.cameraCaptureFailures));
    status.insert(QStringLiteral("camera_encode_failures"),
                  QString::number(m_streamDiagnostics.cameraEncodeFailures));
    status.insert(QStringLiteral("camera_send_failures"),
                  QString::number(m_streamDiagnostics.cameraSendFailures));
    status.insert(QStringLiteral("camera_drops"), QString::number(m_streamDiagnostics.cameraDrops));
    status.insert(QStringLiteral("gripper_installed"), false);
    status.insert(QStringLiteral("gripper_configured"), false);
    status.insert(QStringLiteral("v2_motion_production_enabled"), kV2MotionProductionEnabled);
    status.insert(QStringLiteral("video_fragmentation_production_enabled"),
                  kVideoFragmentationProductionEnabled);
    return status;
}

void RelayRuntime::publishRelayStatus()
{
    const bool ready = relayReady(relayStatusInput());
    if (m_lastReady && !ready) {
        emit logMessage(QStringLiteral(">> relay_ready=false: port disconnected or valid state older than 500 ms."));
    }
    m_lastReady = ready;
    if (m_v2Client && m_v2Handshaken && m_pushRelayStatus) {
        queueV2Object(enrichedRelayStatus(), false, true);
    }
}

void RelayRuntime::publishHeartbeat()
{
    publishRelayStatus();
}

void RelayRuntime::expireRequests()
{
    if (const auto error = m_legacyFramer.expireIncomplete(steadyNowNs())) {
        ++m_diagnostics.legacyParseFailures;
        queueLegacyBytes(QStringLiteral("ERROR|%1|%2;").arg(error->code, error->detail).toUtf8());
        if (m_legacyClient) m_legacyClient->disconnectFromHost();
    }
    const QVector<PendingRequest> expired = m_replyLedger.expire(steadyNowNs());
    for (const PendingRequest& request : expired) {
        const QString name = commandName(request.command);
        const qint64 timeoutMs = robotReplyTimeoutNs(request.command) / 1000000LL;
        if (request.origin == RequestOrigin::V2 && request.sessionId == m_v2SessionId) {
            QJsonObject error = makeError(
                request.requestId,
                request.sessionId,
                QStringLiteral("robot_reply_timeout"),
                QStringLiteral("command_name=%1,timeout_ms=%2,no controller text reply received within deadline; any late reply will be orphaned")
                    .arg(name)
                    .arg(timeoutMs));
            error.insert(QStringLiteral("command_name"), name);
            error.insert(QStringLiteral("timeout_ms"), timeoutMs);
            queueV2Object(error);
        } else if (request.origin == RequestOrigin::Legacy8888 &&
                   request.connectionGeneration == m_legacyGeneration) {
            queueLegacyBytes(
                QStringLiteral("ERROR|robot_reply_timeout|command=%1,timeout_ms=%2,late reply will be orphaned;")
                    .arg(name)
                    .arg(timeoutMs)
                    .toUtf8());
        }
    }
}

void RelayRuntime::protocolCloseV2(const QString& requestId,
                                   const QString& code,
                                   const QString& detail)
{
    ++m_diagnostics.v2ProtocolRejects;
    queueV2Object(makeError(requestId, m_v2SessionId, code, detail));
    if (m_v2Client) m_v2Client->disconnectFromHost();
}

void RelayRuntime::markPortChanged(int port)
{
    const qint64 now = steadyNowNs();
    if (port == kDashboardPort) m_diagnostics.port29999ChangedSteadyNs = now;
    else if (port == kMotionPort) m_diagnostics.port30003ChangedSteadyNs = now;
    else if (port == kFeedbackPort) m_diagnostics.port30004ChangedSteadyNs = now;
}

void RelayRuntime::emitFeedbackErrorLimited(const ProtocolError& error)
{
    const qint64 now = steadyNowNs();
    if (now - m_lastFeedbackErrorLogNs < 1000 * 1000 * 1000LL) return;
    m_lastFeedbackErrorLogNs = now;
    emit logMessage(QStringLiteral(">> [30004] %1: %2 (high-rate errors are rate-limited)")
                        .arg(error.code, error.detail));
}

} // namespace relay
