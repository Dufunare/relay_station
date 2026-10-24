#include "relay_core.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QReadLocker>
#include <QWriteLocker>
#include <QtEndian>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

namespace relay {
namespace {

constexpr qsizetype kHeaderProbeBytes = 56;
constexpr qsizetype kResyncTailBytes = 49;
constexpr qsizetype kMaxCommandNameBytes = 128;

bool validTargetPort(int port)
{
    return port == kDashboardPort || port == kMotionPort;
}

bool isNameByte(char c)
{
    const auto u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
           (u >= '0' && u <= '9') || c == '_';
}

bool prefixCouldBecome(const QByteArray& bytes, const QByteArray& prefix)
{
    return bytes.size() < prefix.size() && prefix.startsWith(bytes);
}

quint16 readU16(const QByteArray& bytes, qsizetype offset)
{
    return qFromLittleEndian<quint16>(
        reinterpret_cast<const uchar*>(bytes.constData() + offset));
}

quint64 readU64(const QByteArray& bytes, qsizetype offset)
{
    return qFromLittleEndian<quint64>(
        reinterpret_cast<const uchar*>(bytes.constData() + offset));
}

quint8 readU8(const QByteArray& bytes, qsizetype offset)
{
    return static_cast<quint8>(bytes.at(offset));
}

double readDouble(const QByteArray& bytes, qsizetype offset)
{
    const quint64 bits = readU64(bytes, offset);
    double value = 0.0;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

template <std::size_t N>
bool readFiniteArray(const QByteArray& bytes,
                     qsizetype offset,
                     std::array<double, N>* values)
{
    for (std::size_t i = 0; i < N; ++i) {
        const double value = readDouble(bytes, offset + static_cast<qsizetype>(i * 8));
        if (!std::isfinite(value)) {
            return false;
        }
        (*values)[i] = value;
    }
    return true;
}

template <std::size_t N>
QJsonArray jsonArray(const std::array<double, N>& values)
{
    QJsonArray array;
    for (double value : values) {
        array.append(value);
    }
    return array;
}

bool parseFlag(const QByteArray& bytes, qsizetype offset, bool* value)
{
    const quint8 raw = readU8(bytes, offset);
    if (raw > 1) {
        return false;
    }
    *value = raw == 1;
    return true;
}

bool parseUnsigned(const QString& text, quint64* value)
{
    if (!isDecimalId(text)) {
        return false;
    }
    bool ok = false;
    const quint64 parsed = text.toULongLong(&ok, 10);
    if (!ok) {
        return false;
    }
    *value = parsed;
    return true;
}

int controllerCode(const QByteArray& rawReply)
{
    const qsizetype comma = rawReply.indexOf(',');
    const QByteArray first = (comma < 0 ? rawReply : rawReply.left(comma)).trimmed();
    bool ok = false;
    const int code = first.toInt(&ok);
    return ok ? code : std::numeric_limits<int>::min();
}

} // namespace

qint64 steadyNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

qint64 unixNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool isDecimalId(const QString& value)
{
    if (value.isEmpty() || value.size() > 20) {
        return false;
    }
    for (const QChar c : value) {
        if (!c.isDigit()) {
            return false;
        }
    }
    bool ok = false;
    value.toULongLong(&ok, 10);
    return ok;
}

QString commandName(const QByteArray& command)
{
    const QByteArray trimmed = command.trimmed();
    const qsizetype open = trimmed.indexOf('(');
    if (open <= 0 || open > kMaxCommandNameBytes) {
        return {};
    }
    const QByteArray name = trimmed.left(open).trimmed();
    if (name.isEmpty() ||
        std::any_of(name.cbegin(), name.cend(), [](char c) { return !isNameByte(c); })) {
        return {};
    }
    return QString::fromLatin1(name);
}

bool commandExpectsControllerReply(const QByteArray& command)
{
    // Both ServoP and ServoJ are high-rate online servo commands:
    // neither waits on the relay's command/response ledger. Some firmware
    // still sends ServoJ success echoes; these are handled asynchronously
    // and must not be allowed to consume a queued MovL response.
    const QString name=commandName(command);
    return name.compare(QStringLiteral("ServoP"),Qt::CaseInsensitive)!=0 &&
           name.compare(QStringLiteral("ServoJ"),Qt::CaseInsensitive)!=0;
}

qint64 robotReplyTimeoutNs(const QByteArray& command)
{
    return commandName(command).compare(QStringLiteral("EnableRobot"), Qt::CaseInsensitive) == 0
        ? kEnableRobotReplyTimeoutNs
        : kRobotReplyTimeoutNs;
}

LegacyFeedResult LegacyCommandFramer::fail(const QString& code, const QString& detail)
{
    m_failed = true;
    m_buffer.clear();
    LegacyFeedResult result;
    result.errors.append({code, detail});
    result.fatal = true;
    return result;
}

LegacyFeedResult LegacyCommandFramer::feed(const QByteArray& bytes, qint64 nowNs)
{
    if (m_failed) {
        return fail(QStringLiteral("parser_closed"),
                    QStringLiteral("legacy parser must be reset after a fatal error"));
    }
    if (nowNs == 0) {
        nowNs = steadyNowNs();
    }
    if (!bytes.isEmpty()) {
        m_buffer.append(bytes);
        m_lastProgressNs = nowNs;
    }
    if (m_buffer.size() > kMaxLegacyBufferBytes) {
        return fail(QStringLiteral("legacy_buffer_too_large"),
                    QStringLiteral("8888 receive buffer exceeded 256 KiB"));
    }

    LegacyFeedResult result;
    const QByteArray dashboardPrefix = QByteArray::number(kDashboardPort) + '|';
    const QByteArray motionPrefix = QByteArray::number(kMotionPort) + '|';

    while (!m_buffer.isEmpty()) {
        while (!m_buffer.isEmpty() &&
               (m_buffer.front() == '\r' || m_buffer.front() == '\n' ||
                m_buffer.front() == ' ' || m_buffer.front() == '\t')) {
            m_buffer.remove(0, 1);
        }
        if (m_buffer.isEmpty()) {
            break;
        }

        int targetPort = 0;
        qsizetype bodyStart = 0;
        if (m_buffer.startsWith(dashboardPrefix)) {
            targetPort = kDashboardPort;
            bodyStart = dashboardPrefix.size();
        } else if (m_buffer.startsWith(motionPrefix)) {
            targetPort = kMotionPort;
            bodyStart = motionPrefix.size();
        } else if (prefixCouldBecome(m_buffer, dashboardPrefix) ||
                   prefixCouldBecome(m_buffer, motionPrefix)) {
            break;
        } else {
            return fail(QStringLiteral("invalid_target_port"),
                        QStringLiteral("8888 frame does not start with 29999| or 30003|"));
        }

        qsizetype open = -1;
        for (qsizetype i = bodyStart; i < m_buffer.size(); ++i) {
            const char c = m_buffer.at(i);
            if (c == '(') {
                open = i;
                break;
            }
            if ((i - bodyStart) > kMaxCommandNameBytes ||
                !(isNameByte(c) || c == ' ' || c == '\t')) {
                return fail(QStringLiteral("invalid_command_boundary"),
                            QStringLiteral("8888 command name or opening parenthesis is invalid"));
            }
        }
        if (open < 0) {
            if (m_buffer.size() - bodyStart > kMaxCommandNameBytes) {
                return fail(QStringLiteral("invalid_command_boundary"),
                            QStringLiteral("8888 command has no opening parenthesis"));
            }
            break;
        }
        if (commandName(m_buffer.mid(bodyStart, open - bodyStart + 1)).isEmpty()) {
            return fail(QStringLiteral("invalid_command_name"),
                        QStringLiteral("8888 command name is empty or malformed"));
        }

        int depth = 0;
        char quote = 0;
        bool escaped = false;
        qsizetype end = -1;
        for (qsizetype i = open; i < m_buffer.size(); ++i) {
            const char c = m_buffer.at(i);
            if (quote != 0) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == quote) {
                    quote = 0;
                }
                continue;
            }
            if (c == '\'' || c == '"') {
                quote = c;
            } else if (c == '(') {
                ++depth;
            } else if (c == ')') {
                --depth;
                if (depth < 0) {
                    return fail(QStringLiteral("invalid_command_boundary"),
                                QStringLiteral("8888 command has an unmatched right parenthesis"));
                }
                if (depth == 0) {
                    end = i;
                    break;
                }
            }
        }

        if (end < 0) {
            if (m_buffer.size() - bodyStart > kMaxLegacyCommandBytes) {
                return fail(QStringLiteral("legacy_command_too_large"),
                            QStringLiteral("8888 command exceeded 64 KiB"));
            }
            break;
        }

        const QByteArray command = m_buffer.mid(bodyStart, end - bodyStart + 1).trimmed();
        if (command.size() > kMaxLegacyCommandBytes) {
            return fail(QStringLiteral("legacy_command_too_large"),
                        QStringLiteral("8888 command exceeded 64 KiB"));
        }
        result.commands.append({targetPort, command});
        m_buffer.remove(0, end + 1);
    }
    return result;
}

std::optional<ProtocolError> LegacyCommandFramer::expireIncomplete(qint64 nowNs,
                                                                    qint64 timeoutNs)
{
    if (!m_failed && !m_buffer.isEmpty() && m_lastProgressNs > 0 &&
        nowNs - m_lastProgressNs >= timeoutNs) {
        m_failed = true;
        m_buffer.clear();
        return ProtocolError{QStringLiteral("legacy_frame_timeout"),
                             QStringLiteral("8888 command remained incomplete too long")};
    }
    return std::nullopt;
}

void LegacyCommandFramer::reset()
{
    m_buffer.clear();
    m_lastProgressNs = 0;
    m_failed = false;
}

ReplyFeedResult RobotReplyFramer::feed(const QByteArray& bytes)
{
    ReplyFeedResult result;
    if (m_failed) {
        result.errors.append({QStringLiteral("parser_closed"),
                              QStringLiteral("robot reply parser requires reset")});
        result.fatal = true;
        return result;
    }
    m_buffer.append(bytes);
    if (m_buffer.size() > kMaxRobotReplyBufferBytes) {
        m_buffer.clear();
        m_failed = true;
        result.errors.append({QStringLiteral("reply_buffer_too_large"),
                              QStringLiteral("robot reply buffer exceeded 256 KiB")});
        result.fatal = true;
        return result;
    }
    for (;;) {
        const qsizetype semicolon = m_buffer.indexOf(';');
        if (semicolon < 0) {
            if (m_buffer.size() > kMaxRobotReplyBytes) {
                m_buffer.clear();
                m_failed = true;
                result.errors.append({QStringLiteral("reply_too_large"),
                                      QStringLiteral("robot reply exceeded 64 KiB")});
                result.fatal = true;
            }
            break;
        }
        QByteArray reply = m_buffer.left(semicolon + 1);
        m_buffer.remove(0, semicolon + 1);
        while (!m_buffer.isEmpty() && (m_buffer.front() == '\r' || m_buffer.front() == '\n')) {
            m_buffer.remove(0, 1);
        }
        if (reply.size() > kMaxRobotReplyBytes) {
            m_failed = true;
            m_buffer.clear();
            result.errors.append({QStringLiteral("reply_too_large"),
                                  QStringLiteral("robot reply exceeded 64 KiB")});
            result.fatal = true;
            break;
        }
        result.replies.append(reply);
    }
    return result;
}

void RobotReplyFramer::reset()
{
    m_buffer.clear();
    m_failed = false;
}

bool ShortWriteBuffer::enqueue(const QByteArray& bytes)
{
    if (bytes.isEmpty() || bytes.size() > m_maxBytes - m_buffer.size()) {
        return false;
    }
    m_buffer.append(bytes);
    return true;
}

qint64 ShortWriteBuffer::drain(const std::function<qint64(const char*, qint64)>& writer)
{
    qint64 total = 0;
    while (!m_buffer.isEmpty()) {
        const qint64 written = writer(m_buffer.constData(), m_buffer.size());
        if (written <= 0) {
            break;
        }
        const qsizetype consumed = std::min<qsizetype>(written, m_buffer.size());
        m_buffer.remove(0, consumed);
        total += consumed;
    }
    return total;
}

PendingReplyLedger::PortQueue* PendingReplyLedger::queueFor(int targetPort)
{
    if (targetPort == kDashboardPort) {
        return &m_dashboard;
    }
    if (targetPort == kMotionPort) {
        return &m_motion;
    }
    return nullptr;
}

const PendingReplyLedger::PortQueue* PendingReplyLedger::queueFor(int targetPort) const
{
    return const_cast<PendingReplyLedger*>(this)->queueFor(targetPort);
}

bool PendingReplyLedger::enqueue(int targetPort, const PendingRequest& request)
{
    PortQueue* queue = queueFor(targetPort);
    if (!queue || request.deadlineSteadyNs <= request.relayReceiveSteadyNs) {
        return false;
    }
    queue->pending.enqueue(request);
    return true;
}

ReplyMatch PendingReplyLedger::match(int targetPort,
                                     const QByteArray& rawReply,
                                     qint64 receiveSteadyNs)
{
    ReplyMatch result;
    result.rawReply = rawReply;
    result.robotReplyReceiveSteadyNs = receiveSteadyNs;
    PortQueue* queue = queueFor(targetPort);
    if (!queue) {
        result.orphan = true;
        return result;
    }
    if (queue->orphanDebt > 0) {
        --queue->orphanDebt;
        ++queue->orphanCount;
        result.orphan = true;
        return result;
    }
    if (queue->pending.isEmpty()) {
        ++queue->orphanCount;
        result.orphan = true;
        return result;
    }
    result.request = queue->pending.dequeue();
    return result;
}

QVector<PendingRequest> PendingReplyLedger::expire(qint64 nowNs)
{
    QVector<PendingRequest> expired;
    for (PortQueue* queue : {&m_dashboard, &m_motion}) {
        while (!queue->pending.isEmpty() && queue->pending.head().deadlineSteadyNs <= nowNs) {
            expired.append(queue->pending.dequeue());
            ++queue->timeoutCount;
            ++queue->orphanDebt;
        }
    }
    return expired;
}

void PendingReplyLedger::clearConnection(RequestOrigin origin, quint64 generation)
{
    for (PortQueue* queue : {&m_dashboard, &m_motion}) {
        QQueue<PendingRequest> kept;
        while (!queue->pending.isEmpty()) {
            const PendingRequest request = queue->pending.dequeue();
            if (request.origin != origin || request.connectionGeneration != generation) {
                kept.enqueue(request);
            }
        }
        queue->pending.swap(kept);
    }
}

void PendingReplyLedger::clearSession(const QString& sessionId)
{
    for (PortQueue* queue : {&m_dashboard, &m_motion}) {
        QQueue<PendingRequest> kept;
        while (!queue->pending.isEmpty()) {
            const PendingRequest request = queue->pending.dequeue();
            if (request.sessionId != sessionId) {
                kept.enqueue(request);
            }
        }
        queue->pending.swap(kept);
    }
}

QVector<PendingRequest> PendingReplyLedger::clearPort(int targetPort)
{
    QVector<PendingRequest> removed;
    PortQueue* queue = queueFor(targetPort);
    if (!queue) {
        return removed;
    }
    while (!queue->pending.isEmpty()) removed.append(queue->pending.dequeue());
    queue->pending.clear();
    queue->orphanDebt = 0;
    return removed;
}

void PendingReplyLedger::clearAll()
{
    m_dashboard.pending.clear();
    m_motion.pending.clear();
    m_dashboard.orphanDebt = 0;
    m_motion.orphanDebt = 0;
}

qsizetype PendingReplyLedger::pendingCount(int targetPort) const
{
    const PortQueue* queue = queueFor(targetPort);
    return queue ? queue->pending.size() : 0;
}

bool PendingReplyLedger::hasV2Request(const QString& sessionId, const QString& requestId) const
{
    for (const PortQueue* queue : {&m_dashboard, &m_motion}) {
        for (const PendingRequest& request : queue->pending) {
            if (request.origin == RequestOrigin::V2 && request.sessionId == sessionId &&
                request.requestId == requestId) {
                return true;
            }
        }
    }
    return false;
}

quint64 PendingReplyLedger::orphanCount(int targetPort) const
{
    const PortQueue* queue = queueFor(targetPort);
    return queue ? queue->orphanCount : 0;
}

quint64 PendingReplyLedger::timeoutCount(int targetPort) const
{
    const PortQueue* queue = queueFor(targetPort);
    return queue ? queue->timeoutCount : 0;
}

FeedbackStreamParser::FeedbackStreamParser(qsizetype maxBufferBytes)
    : m_maxBufferBytes(std::max<qsizetype>(maxBufferBytes, kFeedbackFrameSize + kResyncTailBytes))
{
}

void FeedbackStreamParser::connected()
{
    ++m_connectionGeneration;
    m_buffer.clear();
}

void FeedbackStreamParser::disconnected()
{
    m_buffer.clear();
}

qsizetype FeedbackStreamParser::findHeader() const
{
    for (qsizetype offset = 0; offset + kHeaderProbeBytes <= m_buffer.size(); ++offset) {
        if (readU16(m_buffer, offset) == kFeedbackFrameSize &&
            readU64(m_buffer, offset + 48) == kFeedbackTestValue) {
            return offset;
        }
    }
    return -1;
}

bool FeedbackStreamParser::parseFrame(const QByteArray& frame,
                                      RobotStateSnapshot* state,
                                      ProtocolError* error)
{
    if (frame.size() != kFeedbackFrameSize || readU16(frame, 0) != kFeedbackFrameSize) {
        ++m_stats.messageSizeErrors;
        *error = {QStringLiteral("invalid_message_size"),
                  QStringLiteral("30004 MessageSize is not 1440")};
        return false;
    }
    if (readU64(frame, 48) != kFeedbackTestValue) {
        ++m_stats.testValueErrors;
        *error = {QStringLiteral("invalid_test_value"),
                  QStringLiteral("30004 TestValue does not match the V3 marker")};
        return false;
    }

    RobotStateSnapshot parsed;
    parsed.valid = true;
    parsed.connectionGeneration = m_connectionGeneration;
    parsed.controllerTimestamp = readU64(frame, 32);
    parsed.messageSize = readU16(frame, 0);
    parsed.testValue = readU64(frame, 48);
    parsed.robotMode = readU64(frame, 24);
    if (parsed.robotMode > static_cast<quint64>(std::numeric_limits<qint64>::max())) {
        *error = {QStringLiteral("invalid_robot_mode"),
                  QStringLiteral("30004 robot_mode cannot be represented safely")};
        return false;
    }
    parsed.user = readU8(frame, 1012);
    parsed.tool = readU8(frame, 1013);

    const bool finite =
        readFiniteArray(frame, 192, &parsed.qTarget) &&
        readFiniteArray(frame, 432, &parsed.qActual) &&
        readFiniteArray(frame, 480, &parsed.qdActual) &&
        readFiniteArray(frame, 528, &parsed.iActual) &&
        readFiniteArray(frame, 624, &parsed.toolVectorActual) &&
        readFiniteArray(frame, 672, &parsed.tcpSpeedActual) &&
        readFiniteArray(frame, 720, &parsed.tcpForce) &&
        readFiniteArray(frame, 768, &parsed.toolVectorTarget) &&
        readFiniteArray(frame, 1304, &parsed.sixForceValue) &&
        readFiniteArray(frame, 1384, &parsed.actualQuaternion) &&
        readFiniteArray(frame, 1176, &parsed.centerXyz);
    parsed.load = readDouble(frame, 1168);
    if (!finite || !std::isfinite(parsed.load)) {
        ++m_stats.nonFiniteErrors;
        *error = {QStringLiteral("non_finite_state"),
                  QStringLiteral("30004 required floating-point field is NaN or Infinity")};
        return false;
    }

    const bool flagsValid =
        parseFlag(frame, 1014, &parsed.runQueuedCmd) &&
        parseFlag(frame, 1015, &parsed.pauseCmdFlag) &&
        parseFlag(frame, 1026, &parsed.enableStatus) &&
        parseFlag(frame, 1027, &parsed.dragStatus) &&
        parseFlag(frame, 1028, &parsed.runningStatus) &&
        parseFlag(frame, 1029, &parsed.errorStatus) &&
        parseFlag(frame, 1030, &parsed.jogStatus) &&
        parseFlag(frame, 1037, &parsed.sixForceOnline);
    if (!flagsValid) {
        ++m_stats.invalidFlagErrors;
        *error = {QStringLiteral("invalid_state_flag"),
                  QStringLiteral("30004 boolean status field is not 0 or 1")};
        return false;
    }
    *state = parsed;
    return true;
}

FeedbackFeedResult FeedbackStreamParser::feed(const QByteArray& bytes,
                                               qint64 receiveSteadyNs,
                                               qint64 receiveUnixNs)
{
    FeedbackFeedResult result;
    if (receiveSteadyNs == 0) {
        receiveSteadyNs = steadyNowNs();
    }
    if (receiveUnixNs == 0) {
        receiveUnixNs = unixNowNs();
    }
    m_stats.bytesReceived += static_cast<quint64>(bytes.size());
    m_buffer.append(bytes);

    while (!m_buffer.isEmpty()) {
        if (m_buffer.size() < kHeaderProbeBytes) {
            break;
        }
        const qsizetype header = findHeader();
        if (header < 0) {
            ++m_stats.invalidFrames;
            if (readU16(m_buffer, 0) == kFeedbackFrameSize) {
                ++m_stats.testValueErrors;
                result.errors.append({QStringLiteral("invalid_test_value"),
                                      QStringLiteral("30004 candidate has an invalid TestValue")});
            } else {
                ++m_stats.messageSizeErrors;
                result.errors.append({QStringLiteral("invalid_message_size"),
                                      QStringLiteral("30004 stream has no valid frame header")});
            }
            const qsizetype discard = std::max<qsizetype>(0, m_buffer.size() - kResyncTailBytes);
            if (discard > 0) {
                m_buffer.remove(0, discard);
                m_stats.misalignedBytes += static_cast<quint64>(discard);
                ++m_stats.resyncCount;
            }
            break;
        }
        if (header > 0) {
            m_buffer.remove(0, header);
            m_stats.misalignedBytes += static_cast<quint64>(header);
            ++m_stats.resyncCount;
        }
        if (m_buffer.size() < kFeedbackFrameSize) {
            break;
        }
        const QByteArray frame = m_buffer.left(kFeedbackFrameSize);
        m_buffer.remove(0, kFeedbackFrameSize);
        RobotStateSnapshot state;
        ProtocolError error;
        if (!parseFrame(frame, &state, &error)) {
            ++m_stats.invalidFrames;
            result.errors.append(error);
            continue;
        }
        state.stateSeq = ++m_stateSeq;
        state.relayReceiveSteadyNs = receiveSteadyNs;
        state.relayReceiveUnixNs = receiveUnixNs;
        ++m_stats.validFrames;
        result.states.append(state);
    }

    if (m_buffer.size() > m_maxBufferBytes) {
        const qsizetype discard = m_buffer.size() - kResyncTailBytes;
        m_buffer.remove(0, discard);
        m_stats.misalignedBytes += static_cast<quint64>(discard);
        ++m_stats.bufferDrops;
        ++m_stats.resyncCount;
        result.errors.append({QStringLiteral("feedback_buffer_limit"),
                              QStringLiteral("30004 buffer limit reached; stale bytes discarded")});
    }
    return result;
}

void RobotStateStore::publish(const RobotStateSnapshot& state)
{
    QWriteLocker locker(&m_lock);
    m_latest = state;
}

void RobotStateStore::invalidate(quint64 connectionGeneration)
{
    QWriteLocker locker(&m_lock);
    m_latest.valid = false;
    m_latest.connectionGeneration = connectionGeneration;
}

RobotStateSnapshot RobotStateStore::latest() const
{
    QReadLocker locker(&m_lock);
    return m_latest;
}

V2FeedResult V2FrameParser::fail(const QString& code, const QString& detail)
{
    m_failed = true;
    m_buffer.clear();
    V2FeedResult result;
    result.errors.append({code, detail});
    result.fatal = true;
    return result;
}

V2FeedResult V2FrameParser::feed(const QByteArray& bytes)
{
    if (m_failed) {
        return fail(QStringLiteral("parser_closed"),
                    QStringLiteral("v2 parser must be reset after a fatal error"));
    }
    m_buffer.append(bytes);
    V2FeedResult result;
    while (m_buffer.size() >= 4) {
        const quint32 bodySize = qFromBigEndian<quint32>(
            reinterpret_cast<const uchar*>(m_buffer.constData()));
        if (bodySize == 0) {
            return fail(QStringLiteral("zero_length"), QStringLiteral("v2 body length cannot be zero"));
        }
        if (bodySize > kMaxV2BodyBytes) {
            return fail(QStringLiteral("body_too_large"), QStringLiteral("v2 body exceeds 1 MiB"));
        }
        if (m_buffer.size() < static_cast<qsizetype>(4 + bodySize)) {
            break;
        }
        const QByteArray body = m_buffer.mid(4, bodySize);
        m_buffer.remove(0, 4 + bodySize);
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            return fail(QStringLiteral("invalid_json"), QStringLiteral("v2 body is not a JSON object"));
        }
        const QJsonObject object = document.object();
        if (!object.contains(QStringLiteral("v")) || !object.value(QStringLiteral("v")).isDouble() ||
            object.value(QStringLiteral("v")).toInt(-1) != 2) {
            return fail(QStringLiteral("invalid_version"), QStringLiteral("v2 message requires numeric v=2"));
        }
        const QJsonValue typeValue = object.value(QStringLiteral("type"));
        const QJsonValue requestValue = object.value(QStringLiteral("request_id"));
        const QJsonValue sessionValue = object.value(QStringLiteral("session_id"));
        if (!typeValue.isString() || !requestValue.isString() || !sessionValue.isString()) {
            return fail(QStringLiteral("missing_common_field"),
                        QStringLiteral("v2 type, request_id and session_id are required strings"));
        }
        const QString type = typeValue.toString();
        static const QStringList types = {
            QStringLiteral("hello"), QStringLiteral("hello_ack"), QStringLiteral("command"),
            QStringLiteral("reply"), QStringLiteral("robot_state"), QStringLiteral("relay_status"),
            QStringLiteral("stream_control"), QStringLiteral("error")};
        if (!types.contains(type)) {
            return fail(QStringLiteral("invalid_type"), QStringLiteral("unsupported v2 message type"));
        }
        if (!isDecimalId(requestValue.toString())) {
            return fail(QStringLiteral("invalid_request_id"),
                        QStringLiteral("v2 request_id must be an unsigned decimal string"));
        }
        if (sessionValue.toString().size() > 128) {
            return fail(QStringLiteral("invalid_session_id"),
                        QStringLiteral("v2 session_id exceeds 128 characters"));
        }
        result.messages.append(object);
    }
    if (m_buffer.size() > static_cast<qsizetype>(kMaxV2BodyBytes + 4)) {
        return fail(QStringLiteral("receive_buffer_limit"),
                    QStringLiteral("v2 receive buffer exceeded its bound"));
    }
    return result;
}

void V2FrameParser::reset()
{
    m_buffer.clear();
    m_failed = false;
}

QByteArray encodeV2Frame(const QJsonObject& object)
{
    const QByteArray body = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (body.isEmpty() || body.size() > kMaxV2BodyBytes) {
        return {};
    }
    QByteArray frame(4, Qt::Uninitialized);
    qToBigEndian<quint32>(static_cast<quint32>(body.size()),
                          reinterpret_cast<uchar*>(frame.data()));
    frame.append(body);
    return frame;
}

QJsonObject makeError(const QString& requestId,
                      const QString& sessionId,
                      const QString& code,
                      const QString& detail)
{
    return {{QStringLiteral("v"), 2},
            {QStringLiteral("type"), QStringLiteral("error")},
            {QStringLiteral("request_id"), requestId.isEmpty() ? QStringLiteral("0") : requestId},
            {QStringLiteral("session_id"), sessionId},
            {QStringLiteral("code"), code},
            {QStringLiteral("detail"), detail}};
}

QJsonObject robotStateJson(const RobotStateSnapshot& state, const QString& sessionId)
{
    return {{QStringLiteral("v"), 2},
            {QStringLiteral("type"), QStringLiteral("robot_state")},
            {QStringLiteral("request_id"), QStringLiteral("0")},
            {QStringLiteral("session_id"), sessionId},
            {QStringLiteral("valid"), state.valid},
            {QStringLiteral("source"), QStringLiteral("cr3_30004")},
            {QStringLiteral("state_seq"), QString::number(state.stateSeq)},
            {QStringLiteral("controller_timestamp"), QString::number(state.controllerTimestamp)},
            {QStringLiteral("relay_receive_steady_ns"), QString::number(state.relayReceiveSteadyNs)},
            {QStringLiteral("relay_receive_unix_ns"), QString::number(state.relayReceiveUnixNs)},
            {QStringLiteral("message_size"), static_cast<int>(state.messageSize)},
            {QStringLiteral("test_value"), QString::number(state.testValue)},
            {QStringLiteral("robot_mode"), static_cast<qint64>(state.robotMode)},
            {QStringLiteral("q_actual"), jsonArray(state.qActual)},
            {QStringLiteral("qd_actual"), jsonArray(state.qdActual)},
            {QStringLiteral("i_actual"), jsonArray(state.iActual)},
            {QStringLiteral("q_target"), jsonArray(state.qTarget)},
            {QStringLiteral("tool_vector_actual"), jsonArray(state.toolVectorActual)},
            {QStringLiteral("tcp_speed_actual"), jsonArray(state.tcpSpeedActual)},
            {QStringLiteral("tcp_force"), jsonArray(state.tcpForce)},
            {QStringLiteral("tool_vector_target"), jsonArray(state.toolVectorTarget)},
            {QStringLiteral("six_force_value"), jsonArray(state.sixForceValue)},
            {QStringLiteral("actual_quaternion"), jsonArray(state.actualQuaternion)},
            {QStringLiteral("center_xyz"), jsonArray(state.centerXyz)},
            {QStringLiteral("load"), state.load},
            {QStringLiteral("user"), static_cast<int>(state.user)},
            {QStringLiteral("tool"), static_cast<int>(state.tool)},
            {QStringLiteral("enable_status"), state.enableStatus},
            {QStringLiteral("drag_status"), state.dragStatus},
            {QStringLiteral("running_status"), state.runningStatus},
            {QStringLiteral("error_status"), state.errorStatus},
            {QStringLiteral("jog_status"), state.jogStatus},
            {QStringLiteral("run_queued_cmd"), state.runQueuedCmd},
            {QStringLiteral("pause_cmd_flag"), state.pauseCmdFlag},
            {QStringLiteral("six_force_online"), state.sixForceOnline}};
}

bool relayReady(const RelayStatusInput& input)
{
    return input.port29999Connected && input.port30003Connected &&
           input.port30004Connected && input.state.valid &&
           input.state.connectionGeneration > 0 &&
           input.state.relayReceiveSteadyNs > 0 && input.nowSteadyNs >= input.state.relayReceiveSteadyNs &&
           input.nowSteadyNs - input.state.relayReceiveSteadyNs <= kStateFreshnessNs;
}

double stateAgeMs(const RelayStatusInput& input)
{
    constexpr double neverValidAgeMs = 1.0e12;
    if (!input.state.valid || input.state.relayReceiveSteadyNs <= 0 ||
        input.nowSteadyNs < input.state.relayReceiveSteadyNs) {
        return neverValidAgeMs;
    }
    return static_cast<double>(input.nowSteadyNs - input.state.relayReceiveSteadyNs) / 1.0e6;
}

QJsonObject relayStatusJson(const RelayStatusInput& input, const QString& sessionId)
{
    return {{QStringLiteral("v"), 2},
            {QStringLiteral("type"), QStringLiteral("relay_status")},
            {QStringLiteral("request_id"), QStringLiteral("0")},
            {QStringLiteral("session_id"), sessionId},
            {QStringLiteral("port_29999_connected"), input.port29999Connected},
            {QStringLiteral("port_30003_connected"), input.port30003Connected},
            {QStringLiteral("port_30004_connected"), input.port30004Connected},
            {QStringLiteral("relay_ready"), relayReady(input)},
            {QStringLiteral("last_valid_state_age_ms"), stateAgeMs(input)},
            {QStringLiteral("state_seq"), QString::number(input.state.valid ? input.state.stateSeq : 0)}};
}

QString twinPacket(const RobotStateSnapshot& state)
{
    if (!state.valid) {
        return {};
    }
    return QStringLiteral("TWIN:%1,%2,%3,%4,%5,%6|%7,%8,%9,%10,%11,%12\n")
        .arg(state.toolVectorActual[0], 0, 'f', 2)
        .arg(state.toolVectorActual[1], 0, 'f', 2)
        .arg(state.toolVectorActual[2], 0, 'f', 2)
        .arg(state.toolVectorActual[3], 0, 'f', 2)
        .arg(state.toolVectorActual[4], 0, 'f', 2)
        .arg(state.toolVectorActual[5], 0, 'f', 2)
        .arg(state.qActual[0], 0, 'f', 2)
        .arg(state.qActual[1], 0, 'f', 2)
        .arg(state.qActual[2], 0, 'f', 2)
        .arg(state.qActual[3], 0, 'f', 2)
        .arg(state.qActual[4], 0, 'f', 2)
        .arg(state.qActual[5], 0, 'f', 2);
}

QString forcePacket(const RobotStateSnapshot& state)
{
    if (!state.valid) {
        return {};
    }
    double quaternionNormSquared = 0.0;
    for (double value : state.actualQuaternion) quaternionNormSquared += value * value;
    if (!std::isfinite(quaternionNormSquared) || quaternionNormSquared < 1.0e-12) {
        return {};
    }
    return QStringLiteral("FORCE:%1,%2,%3,%4,%5,%6|Q:%7,%8,%9,%10\n")
        .arg(state.sixForceValue[0], 0, 'f', 3)
        .arg(state.sixForceValue[1], 0, 'f', 3)
        .arg(state.sixForceValue[2], 0, 'f', 3)
        .arg(state.sixForceValue[3], 0, 'f', 3)
        .arg(state.sixForceValue[4], 0, 'f', 3)
        .arg(state.sixForceValue[5], 0, 'f', 3)
        .arg(state.actualQuaternion[0], 0, 'f', 9)
        .arg(state.actualQuaternion[1], 0, 'f', 9)
        .arg(state.actualQuaternion[2], 0, 'f', 9)
        .arg(state.actualQuaternion[3], 0, 'f', 9);
}

void LatestActionGate::beginSession(const QString& sessionId)
{
    clear();
    m_sessionId = sessionId;
}

void LatestActionGate::clear()
{
    m_sessionId.clear();
    m_generation.clear();
    m_lastActionSeq.reset();
    m_pending.reset();
}

ActionDecision LatestActionGate::offer(const ActionRequest& action)
{
    ActionDecision decision;
    if (m_sessionId.isEmpty() || action.sessionId != m_sessionId) {
        decision.errorCode = QStringLiteral("session_mismatch");
        return decision;
    }
    quint64 generation = 0;
    quint64 sequence = 0;
    if (!parseUnsigned(action.controlGeneration, &generation) ||
        !parseUnsigned(action.actionSeq, &sequence)) {
        decision.errorCode = QStringLiteral("invalid_action_id");
        return decision;
    }
    if (action.ttlNs <= 0 || action.receiveSteadyNs <= 0 || action.command.isEmpty()) {
        decision.errorCode = QStringLiteral("invalid_action_fields");
        return decision;
    }
    if (m_generation != action.controlGeneration) {
        m_generation = action.controlGeneration;
        m_lastActionSeq.reset();
        m_pending.reset();
    }
    if (m_lastActionSeq.has_value() && sequence <= *m_lastActionSeq) {
        decision.errorCode = QStringLiteral("stale_action_seq");
        return decision;
    }
    decision.replacedOlderPending = m_pending.has_value();
    if (decision.replacedOlderPending) {
        ++m_replacementCount;
    }
    m_lastActionSeq = sequence;
    m_pending = action;
    decision.accepted = true;
    return decision;
}

std::optional<ActionRequest> LatestActionGate::takeForWrite(qint64 nowNs)
{
    if (!m_pending.has_value()) {
        return std::nullopt;
    }
    ActionRequest action = *m_pending;
    m_pending.reset();
    if (nowNs < action.receiveSteadyNs || nowNs - action.receiveSteadyNs >= action.ttlNs) {
        ++m_expiredCount;
        return std::nullopt;
    }
    return action;
}

void LatestFrameQueue::offer(const EncodedCameraFrame& frame)
{
    QWriteLocker locker(&m_lock);
    if (m_latest.has_value()) {
        ++m_replacementCount;
    }
    m_latest = frame;
}

std::optional<EncodedCameraFrame> LatestFrameQueue::take()
{
    QWriteLocker locker(&m_lock);
    std::optional<EncodedCameraFrame> result = m_latest;
    m_latest.reset();
    return result;
}

void LatestFrameQueue::clear()
{
    QWriteLocker locker(&m_lock);
    m_latest.reset();
}

bool LatestFrameQueue::empty() const
{
    QReadLocker locker(&m_lock);
    return !m_latest.has_value();
}

quint64 LatestFrameQueue::replacementCount() const
{
    QReadLocker locker(&m_lock);
    return m_replacementCount;
}

QVector<QByteArray> fragmentVideoFrame(const EncodedCameraFrame& frame, quint16 maxDatagramBytes)
{
    QVector<QByteArray> result;
    if (frame.bytes.isEmpty() || frame.bytes.size() > kMaxFragmentedVideoFrameBytes ||
        frame.metadata.frameSeq == 0 || maxDatagramBytes <= kVideoFragmentHeaderBytes) {
        return result;
    }
    const qsizetype payloadLimit = maxDatagramBytes - kVideoFragmentHeaderBytes;
    const qsizetype chunkCountValue = (frame.bytes.size() + payloadLimit - 1) / payloadLimit;
    if (chunkCountValue <= 0 || chunkCountValue > std::numeric_limits<quint16>::max()) {
        return result;
    }
    const quint16 chunkCount = static_cast<quint16>(chunkCountValue);
    result.reserve(chunkCount);
    for (quint16 index = 0; index < chunkCount; ++index) {
        const qsizetype begin = static_cast<qsizetype>(index) * payloadLimit;
        const QByteArray payload = frame.bytes.mid(begin, payloadLimit);
        QByteArray datagram(kVideoFragmentHeaderBytes, '\0');
        std::memcpy(datagram.data(), "TVF2", 4);
        datagram[4] = 1; // protocol version
        datagram[5] = 0; // flags reserved
        qToBigEndian<quint16>(kVideoFragmentHeaderBytes,
                              reinterpret_cast<uchar*>(datagram.data() + 6));
        qToBigEndian<quint64>(frame.metadata.frameSeq,
                              reinterpret_cast<uchar*>(datagram.data() + 8));
        qToBigEndian<quint64>(static_cast<quint64>(frame.metadata.captureSteadyNs),
                              reinterpret_cast<uchar*>(datagram.data() + 16));
        qToBigEndian<quint32>(static_cast<quint32>(frame.bytes.size()),
                              reinterpret_cast<uchar*>(datagram.data() + 24));
        qToBigEndian<quint16>(index, reinterpret_cast<uchar*>(datagram.data() + 28));
        qToBigEndian<quint16>(chunkCount, reinterpret_cast<uchar*>(datagram.data() + 30));
        qToBigEndian<quint16>(static_cast<quint16>(payload.size()),
                              reinterpret_cast<uchar*>(datagram.data() + 32));
        qToBigEndian<quint16>(static_cast<quint16>(std::clamp(frame.metadata.width, 0, 65535)),
                              reinterpret_cast<uchar*>(datagram.data() + 34));
        qToBigEndian<quint16>(static_cast<quint16>(std::clamp(frame.metadata.height, 0, 65535)),
                              reinterpret_cast<uchar*>(datagram.data() + 36));
        datagram[38] = 1; // JPEG
        datagram.append(payload);
        result.append(datagram);
    }
    return result;
}

std::optional<EncodedCameraFrame> VideoFragmentReassembler::feed(const QByteArray& datagram,
                                                                  ProtocolError* error)
{
    auto fail = [error](const QString& code, const QString& detail) {
        if (error) *error = {code, detail};
        return std::optional<EncodedCameraFrame>{};
    };
    if (datagram.size() < kVideoFragmentHeaderBytes ||
        std::memcmp(datagram.constData(), "TVF2", 4) != 0 ||
        static_cast<quint8>(datagram[4]) != 1 ||
        qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(datagram.constData() + 6)) !=
            kVideoFragmentHeaderBytes) {
        return fail(QStringLiteral("invalid_video_fragment_header"),
                    QStringLiteral("fragment magic, version, or header length is invalid"));
    }
    const quint64 frameSeq = qFromBigEndian<quint64>(
        reinterpret_cast<const uchar*>(datagram.constData() + 8));
    const qint64 captureNs = static_cast<qint64>(qFromBigEndian<quint64>(
        reinterpret_cast<const uchar*>(datagram.constData() + 16)));
    const quint32 frameSize = qFromBigEndian<quint32>(
        reinterpret_cast<const uchar*>(datagram.constData() + 24));
    const quint16 chunkIndex = qFromBigEndian<quint16>(
        reinterpret_cast<const uchar*>(datagram.constData() + 28));
    const quint16 chunkCount = qFromBigEndian<quint16>(
        reinterpret_cast<const uchar*>(datagram.constData() + 30));
    const quint16 payloadSize = qFromBigEndian<quint16>(
        reinterpret_cast<const uchar*>(datagram.constData() + 32));
    if (frameSeq == 0 || frameSize == 0 || frameSize > kMaxFragmentedVideoFrameBytes ||
        chunkCount == 0 || chunkIndex >= chunkCount ||
        datagram.size() != kVideoFragmentHeaderBytes + payloadSize) {
        return fail(QStringLiteral("invalid_video_fragment_fields"),
                    QStringLiteral("fragment index, count, payload, or total size is invalid"));
    }
    if (m_frameSeq != frameSeq) {
        if (m_frameSeq != 0 && m_receivedBytes > 0) ++m_droppedAssemblies;
        clear();
        m_frameSeq = frameSeq;
        m_metadata.frameSeq = frameSeq;
        m_metadata.captureSteadyNs = captureNs;
        m_metadata.width = qFromBigEndian<quint16>(
            reinterpret_cast<const uchar*>(datagram.constData() + 34));
        m_metadata.height = qFromBigEndian<quint16>(
            reinterpret_cast<const uchar*>(datagram.constData() + 36));
        m_metadata.encoding = static_cast<quint8>(datagram[38]) == 1
                                  ? QStringLiteral("jpeg") : QStringLiteral("unknown");
        m_metadata.byteCount = frameSize;
        m_chunks.resize(chunkCount);
        m_received.fill(false, chunkCount);
    }
    if (m_chunks.size() != chunkCount || m_metadata.byteCount != frameSize ||
        m_metadata.captureSteadyNs != captureNs) {
        ++m_droppedAssemblies;
        clear();
        return fail(QStringLiteral("video_fragment_mismatch"),
                    QStringLiteral("fragments with the same sequence disagree on metadata"));
    }
    if (!m_received[chunkIndex]) {
        m_chunks[chunkIndex] = datagram.mid(kVideoFragmentHeaderBytes);
        m_received[chunkIndex] = true;
        m_receivedBytes += payloadSize;
    }
    if (m_receivedBytes != frameSize || m_received.contains(false)) {
        return std::nullopt;
    }
    EncodedCameraFrame frame;
    frame.metadata = m_metadata;
    frame.bytes.reserve(frameSize);
    for (const QByteArray& chunk : m_chunks) frame.bytes.append(chunk);
    if (frame.bytes.size() != frameSize) {
        ++m_droppedAssemblies;
        clear();
        return fail(QStringLiteral("video_frame_size_mismatch"),
                    QStringLiteral("reassembled payload does not match frame_size"));
    }
    clear();
    return frame;
}

void VideoFragmentReassembler::clear()
{
    m_frameSeq = 0;
    m_metadata = {};
    m_chunks.clear();
    m_received.clear();
    m_receivedBytes = 0;
}

ProtocolError GripperAdapter::rejectUnavailableCommand() const
{
    if (!m_config.installed) {
        return {QStringLiteral("gripper_not_installed"),
                QStringLiteral("gripper installed=false; no hardware command was sent")};
    }
    if (!m_config.configured) {
        return {QStringLiteral("gripper_not_configured"),
                QStringLiteral("gripper configured=false; no hardware command was sent")};
    }
    return {QStringLiteral("gripper_adapter_placeholder"),
            QStringLiteral("gripper adapter has no production command implementation")};
}

} // namespace relay
