#pragma once

#include "relay_core.h"

#include <QHostAddress>
#include <QObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <optional>

namespace relay {

struct RuntimeDiagnostics {
    quint64 legacyConnections = 0;
    quint64 legacyDisconnects = 0;
    quint64 legacyFrames = 0;
    quint64 legacyParseFailures = 0;
    quint64 legacyPartialReads = 0;
    quint64 legacyCoalescedReads = 0;
    quint64 v2Connections = 0;
    quint64 v2Disconnects = 0;
    quint64 v2Handshakes = 0;
    quint64 v2ProtocolRejects = 0;
    quint64 v2ReceivedFrames = 0;
    quint64 v2SentFrames = 0;
    quint64 v2StateOverwrites = 0;
    quint64 v2StateRateLimited = 0;
    quint64 v2SendRejects = 0;
    quint64 stopCount = 0;
    quint64 sessionSwitches = 0;
    quint64 robotWriteRejects = 0;
    qint64 port29999ChangedSteadyNs = 0;
    qint64 port30003ChangedSteadyNs = 0;
    qint64 port30004ChangedSteadyNs = 0;
};

struct StreamDiagnostics {
    bool twinSubscribed = false;
    bool forceSubscribed = false;
    bool videoSubscribed = false;
    quint64 twinSent = 0;
    quint64 forceSent = 0;
    quint64 videoSent = 0;
    quint64 cameraFrameSeq = 0;
    qint64 cameraLastCaptureSteadyNs = 0;
    qint64 cameraLastEncodedSteadyNs = 0;
    qint64 cameraLastSendSteadyNs = 0;
    qsizetype cameraLastBytes = 0;
    int cameraLastWidth = 0;
    int cameraLastHeight = 0;
    QString cameraLastEncoding;
    quint64 cameraCaptureFailures = 0;
    quint64 cameraEncodeFailures = 0;
    quint64 cameraSendFailures = 0;
    quint64 cameraDrops = 0;
};

class RelayRuntime : public QObject {
    Q_OBJECT

public:
    explicit RelayRuntime(QObject* parent = nullptr);
    ~RelayRuntime() override;

    bool start(const QString& robotIp,
               quint16 dashboardPort,
               quint16 motionPort,
               quint16 feedbackPort,
               quint16 legacyPort = kLegacyPort,
               quint16 v2Port = kV2Port);
    void stop();

    bool isRunning() const { return m_running; }
    bool legacyListening() const { return m_legacyServer.isListening(); }
    bool v2Listening() const { return m_v2Server.isListening(); }
    quint16 legacyServerPort() const { return m_legacyServer.serverPort(); }
    quint16 v2ServerPort() const { return m_v2Server.serverPort(); }
    RobotStateSnapshot latestState() const { return m_stateStore.latest(); }
    FeedbackStats feedbackStats() const { return m_feedbackParser.stats(); }
    RuntimeDiagnostics diagnostics() const { return m_diagnostics; }
    QString currentSessionId() const { return m_v2SessionId; }
    bool currentSessionHandshaken() const { return m_v2Handshaken; }
    bool portConnected(int port) const;

    void setStreamDiagnostics(const StreamDiagnostics& diagnostics);

signals:
    void logMessage(const QString& message);
    void robotStateAvailable();
    void connectionStatusChanged();
    void legacyClientChanged(bool connected);
    void sessionReset();
    void commandRouted(int targetPort, const QByteArray& command, double relayLatencyMs);
    void robotReplyObserved(int targetPort, const QByteArray& rawReply, double relayLatencyMs);

private slots:
    void acceptLegacyClient();
    void acceptV2Client();
    void readLegacyClient();
    void readV2Client();
    void readDashboardReplies();
    void readMotionReplies();
    void readFeedback();
    void expireRequests();
    void publishHeartbeat();

private:
    static constexpr qsizetype kMaxSocketOutputBytes = 1024 * 1024;
    static constexpr qsizetype kSlowV2ThresholdBytes = 128 * 1024;
    static constexpr bool kV2MotionProductionEnabled = false;

    void configureRobotSocket(QTcpSocket* socket, int port);
    void replaceLegacyClient(QTcpSocket* socket);
    void replaceV2Client(QTcpSocket* socket);
    void closeLegacyClient();
    void closeV2Client();
    void invalidateV2Session();
    void clearSessionData();
    void routeLegacyCommand(const LegacyCommand& command, qint64 receivedNs);
    void handleV2Message(const QJsonObject& object, qint64 receivedNs);
    void handleHello(const QJsonObject& object);
    void handleV2Command(const QJsonObject& object, qint64 receivedNs);
    void handleStreamControl(const QJsonObject& object);
    void handleRobotReplies(int targetPort, RobotReplyFramer* framer, const QByteArray& bytes);
    void deliverReply(int targetPort, const ReplyMatch& match);
    bool routeToRobot(int targetPort, const QByteArray& command, ShortWriteBuffer* writeBuffer);
    void drainSocket(QTcpSocket* socket, ShortWriteBuffer* writeBuffer);
    bool queueLegacyBytes(const QByteArray& bytes);
    bool queueV2Object(const QJsonObject& object, bool stateUpdate = false, bool statusUpdate = false);
    void drainV2Output();
    void publishRobotState(const RobotStateSnapshot& state);
    void publishRelayStatus();
    RelayStatusInput relayStatusInput() const;
    QJsonObject enrichedRelayStatus() const;
    void protocolCloseV2(const QString& requestId, const QString& code, const QString& detail);
    void markPortChanged(int port);
    void emitFeedbackErrorLimited(const ProtocolError& error);

    QTcpServer m_legacyServer;
    QTcpServer m_v2Server;
    QPointer<QTcpSocket> m_legacyClient;
    QPointer<QTcpSocket> m_v2Client;
    QTcpSocket m_dashboardSocket;
    QTcpSocket m_motionSocket;
    QTcpSocket m_feedbackSocket;
    QTimer m_housekeepingTimer;
    QTimer m_statusHeartbeatTimer;
    QTimer m_stateStaleTimer;
    QTimer m_robotReconnectTimer;

    LegacyCommandFramer m_legacyFramer;
    V2FrameParser m_v2Parser;
    RobotReplyFramer m_dashboardReplyFramer;
    RobotReplyFramer m_motionReplyFramer;
    FeedbackStreamParser m_feedbackParser;
    RobotStateStore m_stateStore;
    PendingReplyLedger m_replyLedger;
    LatestActionGate m_actionGate;
    GripperAdapter m_gripper;
    ShortWriteBuffer m_legacyOutput{kMaxSocketOutputBytes};
    ShortWriteBuffer m_v2Output{kMaxSocketOutputBytes};
    ShortWriteBuffer m_dashboardOutput{kMaxSocketOutputBytes};
    ShortWriteBuffer m_motionOutput{kMaxSocketOutputBytes};

    std::optional<QByteArray> m_latestV2StateFrame;
    V2RobotStatePublishGate m_v2RobotGate;
    RobotStateSnapshot m_lastV2ReportedRobotState;
    bool m_haveV2ReportedRobotState = false;
    std::optional<QByteArray> m_latestV2StatusFrame;
    RuntimeDiagnostics m_diagnostics;
    StreamDiagnostics m_streamDiagnostics;
    QString m_v2SessionId;
    quint64 m_legacyGeneration = 0;
    bool m_v2Handshaken = false;
    bool m_pushRobotState = true;
    bool m_pushRelayStatus = true;
    bool m_running = false;
    bool m_stopping = false;
    bool m_lastReady = false;
    qint64 m_lastFeedbackErrorLogNs = 0;
    QString m_robotIp;
    quint16 m_dashboardPort = kDashboardPort;
    quint16 m_motionPort = kMotionPort;
    quint16 m_feedbackPort = kFeedbackPort;
};

} // namespace relay
