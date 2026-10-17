#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QQueue>
#include <QReadWriteLock>
#include <QString>
#include <QVector>

#include <array>
#include <functional>
#include <optional>

namespace relay {

constexpr int kDashboardPort = 29999;
constexpr int kMotionPort = 30003;
constexpr int kFeedbackPort = 30004;
constexpr int kLegacyPort = 8888;
constexpr int kV2Port = 8891;
constexpr int kFeedbackFrameSize = 1440;
constexpr quint64 kFeedbackTestValue = Q_UINT64_C(0x0123456789ABCDEF);
constexpr qsizetype kMaxLegacyCommandBytes = 64 * 1024;
constexpr qsizetype kMaxLegacyBufferBytes = 256 * 1024;
constexpr qsizetype kMaxRobotReplyBytes = 64 * 1024;
constexpr qsizetype kMaxRobotReplyBufferBytes = 256 * 1024;
constexpr quint32 kMaxV2BodyBytes = 1024 * 1024;
constexpr qint64 kStateFreshnessNs = 500 * 1000 * 1000LL;
constexpr qint64 kIncompleteLegacyTimeoutNs = 2000 * 1000 * 1000LL;
constexpr qint64 kRobotReplyTimeoutNs = 3000 * 1000 * 1000LL;
constexpr qint64 kEnableRobotReplyTimeoutNs = 30000 * 1000 * 1000LL;
constexpr bool kVideoFragmentationProductionEnabled = false;
constexpr quint16 kVideoFragmentHeaderBytes = 40;
constexpr qsizetype kMaxFragmentedVideoFrameBytes = 4 * 1024 * 1024;

qint64 steadyNowNs();
qint64 unixNowNs();
bool isDecimalId(const QString& value);
QString commandName(const QByteArray& command);
bool commandExpectsControllerReply(const QByteArray& command);
qint64 robotReplyTimeoutNs(const QByteArray& command);

struct ProtocolError {
    QString code;
    QString detail;
};

struct LegacyCommand {
    int targetPort = 0;
    QByteArray command;
};

struct LegacyFeedResult {
    QVector<LegacyCommand> commands;
    QVector<ProtocolError> errors;
    bool fatal = false;
};

class LegacyCommandFramer {
public:
    LegacyFeedResult feed(const QByteArray& bytes, qint64 nowSteadyNs = 0);
    std::optional<ProtocolError> expireIncomplete(qint64 nowSteadyNs,
                                                   qint64 timeoutNs = kIncompleteLegacyTimeoutNs);
    void reset();
    qsizetype bufferedBytes() const { return m_buffer.size(); }

private:
    LegacyFeedResult fail(const QString& code, const QString& detail);
    QByteArray m_buffer;
    qint64 m_lastProgressNs = 0;
    bool m_failed = false;
};

struct ReplyFeedResult {
    QVector<QByteArray> replies;
    QVector<ProtocolError> errors;
    bool fatal = false;
};

class RobotReplyFramer {
public:
    ReplyFeedResult feed(const QByteArray& bytes);
    void reset();
    qsizetype bufferedBytes() const { return m_buffer.size(); }

private:
    QByteArray m_buffer;
    bool m_failed = false;
};

class ShortWriteBuffer {
public:
    explicit ShortWriteBuffer(qsizetype maxBytes = 1024 * 1024) : m_maxBytes(maxBytes) {}
    bool enqueue(const QByteArray& bytes);
    qint64 drain(const std::function<qint64(const char*, qint64)>& writer);
    void clear() { m_buffer.clear(); }
    qsizetype pendingBytes() const { return m_buffer.size(); }

private:
    qsizetype m_maxBytes;
    QByteArray m_buffer;
};

enum class RequestOrigin { Legacy8888, V2 };

struct PendingRequest {
    RequestOrigin origin = RequestOrigin::Legacy8888;
    quint64 connectionGeneration = 0;
    QString sessionId;
    QString requestId;
    QByteArray command;
    qint64 relayReceiveSteadyNs = 0;
    qint64 deadlineSteadyNs = 0;
};

struct ReplyMatch {
    bool orphan = false;
    std::optional<PendingRequest> request;
    QByteArray rawReply;
    qint64 robotReplyReceiveSteadyNs = 0;
};

class PendingReplyLedger {
public:
    bool enqueue(int targetPort, const PendingRequest& request);
    ReplyMatch match(int targetPort, const QByteArray& rawReply, qint64 receiveSteadyNs);
    QVector<PendingRequest> expire(qint64 nowSteadyNs);
    void clearConnection(RequestOrigin origin, quint64 connectionGeneration);
    void clearSession(const QString& sessionId);
    QVector<PendingRequest> clearPort(int targetPort);
    void clearAll();
    qsizetype pendingCount(int targetPort) const;
    bool hasV2Request(const QString& sessionId, const QString& requestId) const;
    quint64 orphanCount(int targetPort) const;
    quint64 timeoutCount(int targetPort) const;

private:
    struct PortQueue {
        QQueue<PendingRequest> pending;
        quint64 orphanDebt = 0;
        quint64 orphanCount = 0;
        quint64 timeoutCount = 0;
    };
    PortQueue* queueFor(int targetPort);
    const PortQueue* queueFor(int targetPort) const;
    PortQueue m_dashboard;
    PortQueue m_motion;
};

struct RobotStateSnapshot {
    bool valid = false;
    quint64 stateSeq = 0;
    quint64 connectionGeneration = 0;
    quint64 controllerTimestamp = 0;
    qint64 relayReceiveSteadyNs = 0;
    qint64 relayReceiveUnixNs = 0;
    quint16 messageSize = 0;
    quint64 testValue = 0;
    quint64 robotMode = 0;
    std::array<double, 6> qActual{};
    std::array<double, 6> qdActual{};
    std::array<double, 6> iActual{};
    std::array<double, 6> qTarget{};
    std::array<double, 6> toolVectorActual{};
    std::array<double, 6> tcpSpeedActual{};
    std::array<double, 6> tcpForce{};
    std::array<double, 6> toolVectorTarget{};
    std::array<double, 6> sixForceValue{};
    std::array<double, 4> actualQuaternion{};
    std::array<double, 3> centerXyz{};
    double load = 0.0;
    quint8 user = 0;
    quint8 tool = 0;
    bool enableStatus = false;
    bool dragStatus = false;
    bool runningStatus = false;
    bool errorStatus = false;
    bool jogStatus = false;
    bool runQueuedCmd = false;
    bool pauseCmdFlag = false;
    bool sixForceOnline = false;
};

struct FeedbackStats {
    quint64 bytesReceived = 0;
    quint64 validFrames = 0;
    quint64 invalidFrames = 0;
    quint64 messageSizeErrors = 0;
    quint64 testValueErrors = 0;
    quint64 nonFiniteErrors = 0;
    quint64 invalidFlagErrors = 0;
    quint64 misalignedBytes = 0;
    quint64 resyncCount = 0;
    quint64 bufferDrops = 0;
};

struct FeedbackFeedResult {
    QVector<RobotStateSnapshot> states;
    QVector<ProtocolError> errors;
};

class FeedbackStreamParser {
public:
    explicit FeedbackStreamParser(qsizetype maxBufferBytes = 2 * kFeedbackFrameSize);
    void connected();
    void disconnected();
    FeedbackFeedResult feed(const QByteArray& bytes,
                            qint64 receiveSteadyNs = 0,
                            qint64 receiveUnixNs = 0);
    const FeedbackStats& stats() const { return m_stats; }
    quint64 connectionGeneration() const { return m_connectionGeneration; }
    qsizetype bufferedBytes() const { return m_buffer.size(); }

private:
    qsizetype findHeader() const;
    bool parseFrame(const QByteArray& frame, RobotStateSnapshot* state, ProtocolError* error);
    QByteArray m_buffer;
    qsizetype m_maxBufferBytes;
    quint64 m_connectionGeneration = 0;
    quint64 m_stateSeq = 0;
    FeedbackStats m_stats;
};

class RobotStateStore {
public:
    void publish(const RobotStateSnapshot& state);
    void invalidate(quint64 connectionGeneration);
    RobotStateSnapshot latest() const;

private:
    mutable QReadWriteLock m_lock;
    RobotStateSnapshot m_latest;
};

struct V2FeedResult {
    QVector<QJsonObject> messages;
    QVector<ProtocolError> errors;
    bool fatal = false;
};

class V2FrameParser {
public:
    V2FeedResult feed(const QByteArray& bytes);
    void reset();
    qsizetype bufferedBytes() const { return m_buffer.size(); }

private:
    V2FeedResult fail(const QString& code, const QString& detail);
    QByteArray m_buffer;
    bool m_failed = false;
};

QByteArray encodeV2Frame(const QJsonObject& object);
QJsonObject makeError(const QString& requestId,
                      const QString& sessionId,
                      const QString& code,
                      const QString& detail);
QJsonObject robotStateJson(const RobotStateSnapshot& state, const QString& sessionId);

struct RelayStatusInput {
    bool port29999Connected = false;
    bool port30003Connected = false;
    bool port30004Connected = false;
    qint64 nowSteadyNs = 0;
    RobotStateSnapshot state;
};

bool relayReady(const RelayStatusInput& input);
double stateAgeMs(const RelayStatusInput& input);
QJsonObject relayStatusJson(const RelayStatusInput& input, const QString& sessionId);
QString twinPacket(const RobotStateSnapshot& state);
QString forcePacket(const RobotStateSnapshot& state);

struct ActionRequest {
    QString sessionId;
    QString controlGeneration;
    QString actionSeq;
    qint64 receiveSteadyNs = 0;
    qint64 ttlNs = 0;
    QByteArray command;
};

struct ActionDecision {
    bool accepted = false;
    QString errorCode;
    bool replacedOlderPending = false;
};

class LatestActionGate {
public:
    void beginSession(const QString& sessionId);
    void clear();
    ActionDecision offer(const ActionRequest& action);
    std::optional<ActionRequest> takeForWrite(qint64 nowSteadyNs);
    bool hasPending() const { return m_pending.has_value(); }
    quint64 expiredCount() const { return m_expiredCount; }
    quint64 replacementCount() const { return m_replacementCount; }

private:
    QString m_sessionId;
    QString m_generation;
    std::optional<quint64> m_lastActionSeq;
    std::optional<ActionRequest> m_pending;
    quint64 m_expiredCount = 0;
    quint64 m_replacementCount = 0;
};

struct CameraFrameMetadata {
    quint64 frameSeq = 0;
    qint64 captureSteadyNs = 0;
    qint64 encodedSteadyNs = 0;
    qint64 sendSteadyNs = 0;
    int width = 0;
    int height = 0;
    QString encoding;
    qsizetype byteCount = 0;
};

struct EncodedCameraFrame {
    QByteArray bytes;
    CameraFrameMetadata metadata;
};

class LatestFrameQueue {
public:
    void offer(const EncodedCameraFrame& frame);
    std::optional<EncodedCameraFrame> take();
    void clear();
    bool empty() const;
    quint64 replacementCount() const;

private:
    mutable QReadWriteLock m_lock;
    std::optional<EncodedCameraFrame> m_latest;
    quint64 m_replacementCount = 0;
};

QVector<QByteArray> fragmentVideoFrame(const EncodedCameraFrame& frame,
                                       quint16 maxDatagramBytes = 60000);

class VideoFragmentReassembler {
public:
    std::optional<EncodedCameraFrame> feed(const QByteArray& datagram,
                                           ProtocolError* error = nullptr);
    void clear();
    quint64 droppedAssemblies() const { return m_droppedAssemblies; }

private:
    quint64 m_frameSeq = 0;
    CameraFrameMetadata m_metadata;
    QVector<QByteArray> m_chunks;
    QVector<bool> m_received;
    qsizetype m_receivedBytes = 0;
    quint64 m_droppedAssemblies = 0;
};

struct GripperConfig {
    bool installed = false;
    bool configured = false;
};

class GripperAdapter {
public:
    explicit GripperAdapter(GripperConfig config = {}) : m_config(config) {}
    bool available() const { return m_config.installed && m_config.configured; }
    ProtocolError rejectUnavailableCommand() const;

private:
    GripperConfig m_config;
};

} // namespace relay
