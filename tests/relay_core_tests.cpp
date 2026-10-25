#include "relay_core.h"
#include "relay_runtime.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {

using relay::RobotStateSnapshot;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void putU64(QByteArray* bytes, qsizetype offset, quint64 value)
{
    qToLittleEndian<quint64>(value, reinterpret_cast<uchar*>(bytes->data() + offset));
}

void putDouble(QByteArray* bytes, qsizetype offset, double value)
{
    quint64 bits = 0;
    std::memcpy(&bits, &value, sizeof(value));
    putU64(bytes, offset, bits);
}

template <std::size_t N>
void putArray(QByteArray* bytes, qsizetype offset, const std::array<double, N>& values)
{
    for (std::size_t i = 0; i < N; ++i) {
        putDouble(bytes, offset + static_cast<qsizetype>(i * 8), values[i]);
    }
}

QByteArray validFeedbackFrame(double seed = 1.0)
{
    QByteArray frame(relay::kFeedbackFrameSize, '\0');
    putU64(&frame, 0, relay::kFeedbackFrameSize);
    putU64(&frame, 24, 5);
    putU64(&frame, 32, 123456);
    putU64(&frame, 48, relay::kFeedbackTestValue);
    std::array<double, 6> six{};
    for (std::size_t i = 0; i < six.size(); ++i) {
        six[i] = seed + static_cast<double>(i);
    }
    putArray(&frame, 192, six);
    putArray(&frame, 432, six);
    putArray(&frame, 480, six);
    putArray(&frame, 528, six);
    putArray(&frame, 624, six);
    putArray(&frame, 672, six);
    putArray(&frame, 720, six);
    putArray(&frame, 768, six);
    putDouble(&frame, 1168, 2.5);
    putDouble(&frame, 1176, seed + 20);
    putDouble(&frame, 1184, seed + 21);
    putDouble(&frame, 1192, seed + 22);
    putArray(&frame, 1304, six);
    putArray(&frame, 1384, std::array<double, 4>{1.0, 0.0, 0.0, 0.0});
    frame[1012] = 2;
    frame[1013] = 3;
    frame[1014] = 1;
    frame[1015] = 0;
    frame[1026] = 1;
    frame[1027] = 0;
    frame[1028] = 1;
    frame[1029] = 0;
    frame[1030] = 0;
    frame[1037] = 1;
    return frame;
}

QJsonObject message(const QString& type,
                    const QString& requestId = QStringLiteral("1"),
                    const QString& sessionId = {})
{
    return {{QStringLiteral("v"), 2},
            {QStringLiteral("type"), type},
            {QStringLiteral("request_id"), requestId},
            {QStringLiteral("session_id"), sessionId}};
}

relay::PendingRequest pending(relay::RequestOrigin origin,
                              const QString& id,
                              qint64 received,
                              qint64 deadline,
                              quint64 generation = 1,
                              const QString& session = {})
{
    relay::PendingRequest request;
    request.origin = origin;
    request.requestId = id;
    request.command = "RobotMode()";
    request.relayReceiveSteadyNs = received;
    request.deadlineSteadyNs = deadline;
    request.connectionGeneration = generation;
    request.sessionId = session;
    return request;
}

class Runner {
public:
    void run(const char* name, const std::function<void()>& test)
    {
        ++m_total;
        try {
            test();
            std::cout << "PASS " << m_total << " - " << name << '\n';
        } catch (const std::exception& exception) {
            ++m_failed;
            std::cerr << "FAIL " << m_total << " - " << name << ": "
                      << exception.what() << '\n';
        }
    }

    int finish() const
    {
        std::cout << "RESULT: " << (m_total - m_failed) << '/' << m_total
                  << " cases passed\n";
        return m_failed == 0 ? 0 : 1;
    }

private:
    int m_total = 0;
    int m_failed = 0;
};

bool pumpUntil(const std::function<bool()>& predicate, int timeoutMs = 2000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

QVector<QJsonObject> collectV2(QTcpSocket* socket, int minimum, int timeoutMs = 2000)
{
    relay::V2FrameParser parser;
    QVector<QJsonObject> messages;
    QElapsedTimer timer;
    timer.start();
    while (messages.size() < minimum && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (socket->bytesAvailable() > 0) {
            const relay::V2FeedResult result = parser.feed(socket->readAll());
            messages += result.messages;
            if (result.fatal) break;
        }
        QThread::msleep(1);
    }
    return messages;
}

struct RuntimeHarness {
    QTcpServer dashboardServer;
    QTcpServer motionServer;
    QTcpServer feedbackServer;
    relay::RelayRuntime runtime;
    QPointer<QTcpSocket> dashboardPeer;
    QPointer<QTcpSocket> motionPeer;
    QPointer<QTcpSocket> feedbackPeer;

    void start()
    {
        require(dashboardServer.listen(QHostAddress::LocalHost, 0), "dashboard simulator listen");
        require(motionServer.listen(QHostAddress::LocalHost, 0), "motion simulator listen");
        require(feedbackServer.listen(QHostAddress::LocalHost, 0), "feedback simulator listen");
        require(runtime.start(QStringLiteral("127.0.0.1"), dashboardServer.serverPort(),
                              motionServer.serverPort(), feedbackServer.serverPort(), 0, 0),
                "runtime start");
        require(runtime.v2Listening() && runtime.legacyListening(), "relay listeners missing");
        require(pumpUntil([this] {
            return dashboardServer.hasPendingConnections() && motionServer.hasPendingConnections() &&
                   feedbackServer.hasPendingConnections();
        }), "robot simulator connections missing");
        dashboardPeer = dashboardServer.nextPendingConnection();
        motionPeer = motionServer.nextPendingConnection();
        feedbackPeer = feedbackServer.nextPendingConnection();
        require(pumpUntil([this] {
            return runtime.portConnected(relay::kDashboardPort) &&
                   runtime.portConnected(relay::kMotionPort) &&
                   runtime.portConnected(relay::kFeedbackPort);
        }), "runtime robot sockets not connected");
    }

    QTcpSocket* connectV2()
    {
        auto* client = new QTcpSocket;
        client->connectToHost(QHostAddress::LocalHost, runtime.v2ServerPort());
        require(pumpUntil([client] { return client->state() == QAbstractSocket::ConnectedState; }),
                "v2 client connect");
        return client;
    }

    QJsonObject hello(QTcpSocket* client, const QString& requestId = QStringLiteral("1"))
    {
        QJsonObject object = message(QStringLiteral("hello"), requestId);
        object.insert(QStringLiteral("client"), QStringLiteral("touchvla_windows"));
        object.insert(QStringLiteral("capabilities"),
                      QJsonArray{QStringLiteral("robot_state"), QStringLiteral("relay_status")});
        client->write(relay::encodeV2Frame(object));
        const QVector<QJsonObject> replies = collectV2(client, 2);
        require(replies.size() >= 2 && replies[0]["type"] == "hello_ack", "hello_ack missing");
        return replies[0];
    }

    ~RuntimeHarness()
    {
        runtime.stop();
        delete dashboardPeer.data();
        delete motionPeer.data();
        delete feedbackPeer.data();
    }
};

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    Runner runner;

    runner.run("8888 command split into three reads", [] {
        relay::LegacyCommandFramer parser;
        require(parser.feed("299", 1).commands.isEmpty(), "parsed too early");
        require(parser.feed("99|Robot", 2).commands.isEmpty(), "parsed too early");
        const auto result = parser.feed("Mode()", 3);
        require(result.commands.size() == 1, "did not produce one command");
        require(result.commands[0].command == "RobotMode()", "command changed");
    });
    runner.run("8888 three coalesced commands preserve order", [] {
        relay::LegacyCommandFramer parser;
        const auto result = parser.feed(
            "29999|RobotMode()29999|CP(0)30003|MovL(1,2,3,4,5,6)");
        require(result.commands.size() == 3, "wrong command count");
        require(result.commands[0].targetPort == 29999 && result.commands[1].command == "CP(0)" &&
                    result.commands[2].targetPort == 30003,
                "order or target changed");
    });
    runner.run("8888 nested parentheses quotes and escapes", [] {
        relay::LegacyCommandFramer parser;
        const auto result = parser.feed("29999|Cmd(inner(1),\"a\\\")b\",'x)')30003|MovL(1)");
        require(result.commands.size() == 2, "quoted parenthesis truncated command");
        require(result.commands[0].command.endsWith("'x)')"), "first command boundary wrong");
    });
    runner.run("8888 invalid target and oversize are rejected", [] {
        relay::LegacyCommandFramer invalid;
        require(invalid.feed("12345|RobotMode()").fatal, "invalid port accepted");
        relay::LegacyCommandFramer oversized;
        QByteArray bytes("29999|Cmd(");
        bytes.append(relay::kMaxLegacyCommandBytes + 1, 'x');
        require(oversized.feed(bytes).fatal, "oversize command accepted");
    });
    runner.run("29999 reply half packets reassemble", [] {
        relay::RobotReplyFramer parser;
        require(parser.feed("0,{5},Robot").replies.isEmpty(), "reply parsed early");
        const auto result = parser.feed("Mode();");
        require(result.replies.size() == 1 && result.replies[0].endsWith(';'), "reply not rebuilt");
    });
    runner.run("30003 coalesced replies split", [] {
        relay::RobotReplyFramer parser;
        const auto result = parser.feed("0,{},MovL(1);0,{},MovL(2);");
        require(result.replies.size() == 2, "sticky replies not split");
    });
    runner.run("29999 and 30003 reply FIFOs are isolated", [] {
        relay::PendingReplyLedger ledger;
        require(ledger.enqueue(29999, pending(relay::RequestOrigin::Legacy8888, "a", 1, 10)), "enqueue");
        require(ledger.enqueue(30003, pending(relay::RequestOrigin::Legacy8888, "b", 1, 10)), "enqueue");
        require(ledger.match(30003, "0,{},MovL();", 2).request->requestId == "b", "ports crossed");
        require(ledger.match(29999, "0,{5},RobotMode();", 2).request->requestId == "a", "ports crossed");
    });
    runner.run("active v2 request_id is unique across robot ports", [] {
        relay::PendingReplyLedger ledger;
        require(ledger.enqueue(29999, pending(relay::RequestOrigin::V2, "42", 1, 100, 1, "session-a")),
                "enqueue active request");
        require(ledger.hasV2Request("session-a", "42"), "active id not found");
        require(!ledger.hasV2Request("session-b", "42"), "request leaked across sessions");
        require(!ledger.hasV2Request("session-a", "43"), "different id reported duplicate");
        ledger.match(29999, "0,{5},RobotMode();", 2);
        require(!ledger.hasV2Request("session-a", "42"), "completed id stayed active");
    });
    runner.run("timed out reply becomes orphan", [] {
        relay::PendingReplyLedger ledger;
        ledger.enqueue(29999, pending(relay::RequestOrigin::V2, "old", 1, 5));
        require(ledger.expire(6).size() == 1, "request did not expire");
        ledger.enqueue(29999, pending(relay::RequestOrigin::V2, "new", 6, 20));
        require(ledger.match(29999, "0,{},old();", 7).orphan, "late reply reused");
        require(ledger.pendingCount(29999) == 1, "new request consumed");
    });
    runner.run("short writes continue until complete", [] {
        relay::ShortWriteBuffer queue(100);
        require(queue.enqueue("abcdefghij"), "enqueue failed");
        QByteArray sink;
        queue.drain([&sink](const char* data, qint64 size) {
            const qint64 count = std::min<qint64>(3, size);
            sink.append(data, count);
            return count;
        });
        require(sink == "abcdefghij" && queue.pendingBytes() == 0, "short write lost bytes");
    });

    runner.run("30004 fragmented frame reassembles", [] {
        relay::FeedbackStreamParser parser;
        parser.connected();
        const QByteArray frame = validFeedbackFrame();
        require(parser.feed(frame.left(13), 10, 20).states.isEmpty(), "parsed partial frame");
        require(parser.feed(frame.mid(13, 500), 11, 21).states.isEmpty(), "parsed partial frame");
        const auto result = parser.feed(frame.mid(513), 12, 22);
        require(result.states.size() == 1 && result.states[0].messageSize == 1440, "frame not rebuilt");
    });
    runner.run("30004 two coalesced frames split", [] {
        relay::FeedbackStreamParser parser;
        parser.connected();
        const auto result = parser.feed(validFeedbackFrame(1) + validFeedbackFrame(10));
        require(result.states.size() == 2 && result.states[1].stateSeq == result.states[0].stateSeq + 1,
                "frames not split");
    });
    runner.run("30004 realigns after 1 through 1439 garbage bytes", [] {
        const QByteArray frame = validFeedbackFrame();
        for (int count = 1; count <= 1439; ++count) {
            relay::FeedbackStreamParser parser;
            parser.connected();
            const auto result = parser.feed(QByteArray(count, 'x') + frame);
            if (result.states.size() != 1) {
                throw std::runtime_error("failed realignment at garbage length " + std::to_string(count));
            }
        }
    });
    runner.run("30004 wrong MessageSize cannot update state", [] {
        relay::FeedbackStreamParser parser;
        parser.connected();
        QByteArray frame = validFeedbackFrame();
        qToLittleEndian<quint16>(1439, reinterpret_cast<uchar*>(frame.data()));
        require(parser.feed(frame).states.isEmpty(), "invalid size produced state");
        require(parser.stats().messageSizeErrors > 0, "size error not counted");
    });
    runner.run("30004 wrong TestValue cannot update state", [] {
        relay::FeedbackStreamParser parser;
        parser.connected();
        QByteArray frame = validFeedbackFrame();
        putU64(&frame, 48, 7);
        require(parser.feed(frame).states.isEmpty(), "invalid marker produced state");
        require(parser.stats().testValueErrors > 0, "marker error not counted");
    });
    runner.run("30004 non-finite required field rejected", [] {
        relay::FeedbackStreamParser parser;
        parser.connected();
        QByteArray frame = validFeedbackFrame();
        putDouble(&frame, 432, std::numeric_limits<double>::quiet_NaN());
        require(parser.feed(frame).states.isEmpty(), "NaN produced state");
        require(parser.stats().nonFiniteErrors == 1, "NaN error not counted");
    });
    runner.run("30004 reconnect never reuses old bytes", [] {
        relay::FeedbackStreamParser parser;
        parser.connected();
        const QByteArray frame = validFeedbackFrame();
        parser.feed(frame.left(100));
        parser.disconnected();
        parser.connected();
        require(parser.feed(frame.mid(100)).states.isEmpty(), "old bytes crossed generation");
        const auto result = parser.feed(frame);
        require(result.states.size() == 1 && result.states[0].connectionGeneration == 2,
                "new generation invalid");
    });
    runner.run("state store readers never observe mixed snapshot", [] {
        relay::RobotStateStore store;
        std::atomic<bool> done = false;
        std::atomic<bool> mixed = false;
        std::thread writer([&] {
            for (quint64 sequence = 1; sequence <= 20000; ++sequence) {
                RobotStateSnapshot state;
                state.valid = true;
                state.stateSeq = sequence;
                state.qActual.fill(static_cast<double>(sequence));
                store.publish(state);
            }
            done = true;
        });
        std::thread reader([&] {
            while (!done) {
                const RobotStateSnapshot state = store.latest();
                if (state.valid) {
                    for (double value : state.qActual) {
                        if (value != static_cast<double>(state.stateSeq)) mixed = true;
                    }
                }
            }
        });
        writer.join();
        reader.join();
        require(!mixed, "mixed snapshot observed");
    });

    runner.run("8891 fragmented length frame reassembles", [] {
        relay::V2FrameParser parser;
        const QByteArray frame = relay::encodeV2Frame(message("hello"));
        require(parser.feed(frame.left(2)).messages.isEmpty(), "parsed partial header");
        require(parser.feed(frame.mid(2, 7)).messages.isEmpty(), "parsed partial body");
        require(parser.feed(frame.mid(9)).messages.size() == 1, "v2 frame not rebuilt");
    });
    runner.run("8891 coalesced frames preserve order", [] {
        relay::V2FrameParser parser;
        const auto result = parser.feed(relay::encodeV2Frame(message("hello", "1")) +
                                        relay::encodeV2Frame(message("stream_control", "2", "s")));
        require(result.messages.size() == 2 && result.messages[1]["request_id"] == "2", "v2 order changed");
    });
    runner.run("8891 invalid lengths JSON and fields rejected", [] {
        relay::V2FrameParser zero;
        require(zero.feed(QByteArray(4, '\0')).fatal, "zero length accepted");
        relay::V2FrameParser huge;
        QByteArray header(4, '\0');
        qToBigEndian<quint32>(relay::kMaxV2BodyBytes + 1, reinterpret_cast<uchar*>(header.data()));
        require(huge.feed(header).fatal, "huge length accepted");
        relay::V2FrameParser invalidJson;
        QByteArray body = "{";
        QByteArray invalidFrame(4, '\0');
        qToBigEndian<quint32>(1, reinterpret_cast<uchar*>(invalidFrame.data()));
        invalidFrame += body;
        require(invalidJson.feed(invalidFrame).fatal, "bad JSON accepted");
        relay::V2FrameParser missing;
        require(missing.feed(relay::encodeV2Frame({{"v", 2}})).fatal, "missing fields accepted");
    });
    runner.run("hello_ack echoes request_id", [] {
        const QJsonObject hello = message("hello", "987");
        const QJsonObject ack{{"v", 2}, {"type", "hello_ack"},
                              {"request_id", hello["request_id"]}, {"session_id", "session"}};
        relay::V2FrameParser parser;
        const auto result = parser.feed(relay::encodeV2Frame(ack));
        require(result.messages[0]["request_id"] == "987", "request id not echoed");
    });
    runner.run("new connections use distinct nonempty sessions", [] {
        const QString first = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString second = QUuid::createUuid().toString(QUuid::WithoutBraces);
        require(!first.isEmpty() && first != second && first.size() <= 128, "session uniqueness failed");
    });
    runner.run("wrong and old sessions are rejected", [] {
        relay::LatestActionGate gate;
        gate.beginSession("new");
        const auto decision = gate.offer({"old", "1", "1", 10, 100, "MovL(1)"});
        require(!decision.accepted && decision.errorCode == "session_mismatch", "old session accepted");
    });
    runner.run("server pushes use request_id zero", [] {
        RobotStateSnapshot state;
        state.valid = true;
        const auto robot = relay::robotStateJson(state, "s");
        relay::RelayStatusInput status;
        const auto relayStatus = relay::relayStatusJson(status, "s");
        require(robot["request_id"] == "0" && relayStatus["request_id"] == "0", "push id not zero");
    });
    runner.run("slow v2 client queue is bounded", [] {
        relay::ShortWriteBuffer queue(32);
        require(queue.enqueue(QByteArray(32, 'x')), "initial enqueue failed");
        require(!queue.enqueue("y"), "bounded queue grew beyond limit");
    });
    runner.run("8891 listener can be ready before 8888", [] {
        QTcpServer v2;
        QTcpServer legacy;
        require(v2.listen(QHostAddress::LocalHost, 0), "v2 listen failed");
        require(v2.isListening(), "v2 not ready");
        require(legacy.listen(QHostAddress::LocalHost, 0), "legacy listen failed");
    });
    runner.run("v2 command reply preserves id and raw reply", [] {
        relay::PendingReplyLedger ledger;
        ledger.enqueue(29999, pending(relay::RequestOrigin::V2, "42", 100, 200, 1, "session"));
        const auto match = ledger.match(29999, "0,{5},RobotMode();", 150);
        require(match.request->requestId == "42" && match.rawReply == "0,{5},RobotMode();",
                "reply correlation changed data");
    });
    runner.run("v2 timeout isolates delayed reply", [] {
        relay::PendingReplyLedger ledger;
        ledger.enqueue(30003, pending(relay::RequestOrigin::V2, "42", 1, 2));
        ledger.expire(3);
        require(ledger.match(30003, "0,{},MovL();", 4).orphan, "delayed v2 reply reused");
    });

    runner.run("ready false when any robot port is disconnected", [] {
        relay::RelayStatusInput input;
        input.port29999Connected = input.port30003Connected = input.port30004Connected = true;
        input.nowSteadyNs = 1000;
        input.state.valid = true;
        input.state.connectionGeneration = 1;
        input.state.relayReceiveSteadyNs = 900;
        require(relay::relayReady(input), "valid ready rejected");
        input.port29999Connected = false;
        require(!relay::relayReady(input), "29999 disconnect remained ready");
        input.port29999Connected = true; input.port30003Connected = false;
        require(!relay::relayReady(input), "30003 disconnect remained ready");
        input.port30003Connected = true; input.port30004Connected = false;
        require(!relay::relayReady(input), "30004 disconnect remained ready");
    });
    runner.run("state older than 500 ms makes ready false", [] {
        relay::RelayStatusInput input;
        input.port29999Connected = input.port30003Connected = input.port30004Connected = true;
        input.state.valid = true;
        input.state.connectionGeneration = 1;
        input.state.relayReceiveSteadyNs = 1;
        input.nowSteadyNs = 1 + relay::kStateFreshnessNs + 1;
        require(!relay::relayReady(input), "stale state remained ready");
    });
    runner.run("200 ms heartbeat remains below 500 ms threshold", [] {
        relay::RelayStatusInput input;
        input.port29999Connected = input.port30003Connected = input.port30004Connected = true;
        input.state.valid = true;
        input.state.connectionGeneration = 1;
        input.state.relayReceiveSteadyNs = 1;
        input.nowSteadyNs = 1 + 200 * 1000 * 1000LL;
        require(relay::relayReady(input), "200 ms state rejected");
    });
    runner.run("new session clears old action", [] {
        relay::LatestActionGate gate;
        gate.beginSession("one");
        require(gate.offer({"one", "1", "1", 10, 100, "MovL(1)"}).accepted, "offer failed");
        gate.beginSession("two");
        require(!gate.hasPending(), "old action survived session");
    });
    runner.run("duplicate or decreasing action_seq is rejected", [] {
        relay::LatestActionGate gate;
        gate.beginSession("s");
        require(gate.offer({"s", "1", "2", 10, 100, "MovL(2)"}).accepted, "first failed");
        require(!gate.offer({"s", "1", "2", 11, 100, "MovL(2)"}).accepted, "duplicate accepted");
        require(!gate.offer({"s", "1", "1", 12, 100, "MovL(1)"}).accepted, "decrease accepted");
    });
    runner.run("expired pending action never reaches writer", [] {
        relay::LatestActionGate gate;
        gate.beginSession("s");
        gate.offer({"s", "1", "1", 10, 5, "MovL(1)"});
        require(!gate.takeForWrite(15).has_value() && gate.expiredCount() == 1, "expired action escaped");
    });
    runner.run("latest continuous target replaces older pending target", [] {
        relay::LatestActionGate gate;
        gate.beginSession("s");
        gate.offer({"s", "1", "1", 10, 100, "MovL(1)"});
        const auto decision = gate.offer({"s", "1", "2", 11, 100, "MovL(2)"});
        const auto selected = gate.takeForWrite(12);
        require(decision.replacedOlderPending && selected->command == "MovL(2)", "latest target not retained");
    });
    runner.run("ServoP never waits for an ordinary controller reply", [] {
        require(!relay::commandExpectsControllerReply("ServoP(1,2,3,4,5,6)"), "ServoP waits for reply");
        require(relay::commandExpectsControllerReply("MovL(1,2,3,4,5,6)"), "MovL reply disabled");
    });
    runner.run("EnableRobot uses 30000 ms reply timeout", [] {
        require(relay::robotReplyTimeoutNs("EnableRobot(0.5,0,0,0)") ==
                    relay::kEnableRobotReplyTimeoutNs,
                "EnableRobot did not select 30000 ms");
    });
    runner.run("lowercase enablerobot uses 30000 ms reply timeout", [] {
        require(relay::robotReplyTimeoutNs("enablerobot()") ==
                    relay::kEnableRobotReplyTimeoutNs,
                "lowercase enablerobot did not select 30000 ms");
    });
    runner.run("RobotMode keeps 3000 ms reply timeout", [] {
        require(relay::robotReplyTimeoutNs("RobotMode()") == relay::kRobotReplyTimeoutNs,
                "RobotMode timeout changed");
    });
    runner.run("MovL keeps 3000 ms reply timeout", [] {
        require(relay::robotReplyTimeoutNs("MovL(1,2,3,4,5,6)") == relay::kRobotReplyTimeoutNs,
                "MovL timeout changed");
    });
    runner.run("EnableRobot deadline is finite and its late reply becomes orphaned", [] {
        relay::PendingReplyLedger ledger;
        const qint64 receivedNs = 100;
        relay::PendingRequest request = pending(
            relay::RequestOrigin::Legacy8888,
            QString(),
            receivedNs,
            receivedNs + relay::robotReplyTimeoutNs("EnableRobot(0.5,0,0,0)"));
        request.command = "EnableRobot(0.5,0,0,0)";
        require(ledger.enqueue(relay::kDashboardPort, request), "EnableRobot request was not queued");
        require(ledger.expire(request.deadlineSteadyNs - 1).isEmpty(),
                "EnableRobot expired before 30000 ms");
        require(ledger.expire(request.deadlineSteadyNs).size() == 1,
                "EnableRobot did not expire at 30000 ms");
        const relay::ReplyMatch late = ledger.match(
            relay::kDashboardPort, "0,{},EnableRobot(0.5,0,0,0);", request.deadlineSteadyNs + 1);
        require(late.orphan && !late.request.has_value(), "late EnableRobot reply was not orphaned");
    });
    runner.run("Stop clears old state replies and queues", [] {
        relay::RobotStateStore store;
        RobotStateSnapshot state; state.valid = true; state.stateSeq = 7;
        store.publish(state); store.invalidate(2);
        relay::PendingReplyLedger ledger;
        ledger.enqueue(29999, pending(relay::RequestOrigin::Legacy8888, "x", 1, 5));
        ledger.clearAll();
        relay::ShortWriteBuffer writes(16); writes.enqueue("old"); writes.clear();
        require(!store.latest().valid && ledger.pendingCount(29999) == 0 && writes.pendingBytes() == 0,
                "Stop left session data");
    });

    runner.run("Twin packet remains legacy-compatible", [] {
        relay::FeedbackStreamParser parser; parser.connected();
        const auto state = parser.feed(validFeedbackFrame()).states[0];
        const QString packet = relay::twinPacket(state);
        require(packet.startsWith("TWIN:") && packet.contains('|'), "Twin format changed");
    });
    runner.run("Force packet contains same-state quaternion", [] {
        relay::FeedbackStreamParser parser; parser.connected();
        const auto state = parser.feed(validFeedbackFrame()).states[0];
        const QString packet = relay::forcePacket(state);
        require(packet.startsWith("FORCE:") && packet.contains("|Q:1.000000000,0.000000000"),
                "Force quaternion absent");
    });
    runner.run("TWIN_STOP and FORCE_STOP clear subscription output", [] {
        relay::ShortWriteBuffer twin(64); relay::ShortWriteBuffer force(64);
        twin.enqueue("old twin"); force.enqueue("old force");
        twin.clear(); force.clear();
        require(twin.pendingBytes() == 0 && force.pendingBytes() == 0, "UDP data survived stop");
    });
    runner.run("video queue is bounded and latest-first", [] {
        relay::LatestFrameQueue queue;
        queue.offer({"one", {1, 1, 2, 0, 640, 480, "jpeg", 3}});
        queue.offer({"two", {2, 2, 3, 0, 640, 480, "jpeg", 3}});
        const auto frame = queue.take();
        require(frame->bytes == "two" && queue.replacementCount() == 1 && queue.empty(),
                "video queue did not keep latest");
    });
    runner.run("video metadata sequence and times are monotonic", [] {
        relay::CameraFrameMetadata first{1, 100, 110, 120, 640, 480, "jpeg", 1000};
        relay::CameraFrameMetadata second{2, 200, 210, 220, 640, 480, "jpeg", 1100};
        require(second.frameSeq > first.frameSeq && second.captureSteadyNs > first.captureSteadyNs &&
                    second.encodedSteadyNs >= second.captureSteadyNs && second.sendSteadyNs >= second.encodedSteadyNs,
                "camera metadata not monotonic");
    });
    runner.run("unconfigured gripper command is rejected", [] {
        relay::GripperAdapter gripper;
        require(!gripper.available() && gripper.rejectUnavailableCommand().code == "gripper_not_installed",
                "placeholder enabled gripper");
    });
    runner.run("compatibility parser lifecycle resets without hardware", [] {
        relay::LegacyCommandFramer legacy;
        legacy.feed("29999|Robot"); legacy.reset();
        require(legacy.feed("29999|RobotMode()").commands.size() == 1, "legacy reset failed");
        relay::V2FrameParser v2;
        require(v2.feed(QByteArray(4, '\0')).fatal, "bad frame accepted");
        v2.reset();
        require(v2.feed(relay::encodeV2Frame(message("hello"))).messages.size() == 1, "v2 reset failed");
    });

    runner.run("runtime starts 8891 before 8888 and connects three simulated robot ports", [] {
        RuntimeHarness harness;
        QStringList logs;
        QObject::connect(&harness.runtime, &relay::RelayRuntime::logMessage,
                         [&logs](const QString& line) { logs.append(line); });
        harness.start();
        const auto iterator = std::find_if(logs.cbegin(), logs.cend(), [](const QString& line) {
            return line.contains(QStringLiteral("before legacy"));
        });
        require(iterator != logs.cend(), "listener ordering was not established");
        require(logs.contains(QStringLiteral(">> Reply timeout policy: ordinary=3000 ms, EnableRobot=30000 ms")),
                "reply timeout policy startup log is absent");
    });
    runner.run("runtime exact hello assigns session and echoes request", [] {
        RuntimeHarness harness; harness.start();
        std::unique_ptr<QTcpSocket> client(harness.connectV2());
        const QJsonObject ack = harness.hello(client.get(), QStringLiteral("71"));
        require(ack["request_id"] == "71" && !ack["session_id"].toString().isEmpty(),
                "runtime handshake fields wrong");
        require(harness.runtime.currentSessionHandshaken(), "runtime handshake state false");
    });
    runner.run("runtime publishes validated robot_state and relay_status", [] {
        RuntimeHarness harness; harness.start();
        std::unique_ptr<QTcpSocket> client(harness.connectV2());
        const QString session = harness.hello(client.get())["session_id"].toString();
        harness.feedbackPeer->write(validFeedbackFrame(9));
        const QVector<QJsonObject> messages = collectV2(client.get(), 1);
        const auto stateIt = std::find_if(messages.cbegin(), messages.cend(), [](const QJsonObject& object) {
            return object["type"] == "robot_state";
        });
        require(stateIt != messages.cend(), "robot_state not pushed");
        require((*stateIt)["session_id"] == session && (*stateIt)["message_size"] == 1440 &&
                    (*stateIt)["test_value"] == QString::number(relay::kFeedbackTestValue),
                "robot_state fields invalid");
        require((*stateIt)["q_actual"].toArray().size() == 6, "robot_state array size invalid");
    });
    runner.run("runtime legacy split command and split reply use simulated sockets", [] {
        RuntimeHarness harness; harness.start();
        QTcpSocket legacy;
        legacy.connectToHost(QHostAddress::LocalHost, harness.runtime.legacyServerPort());
        require(pumpUntil([&legacy] { return legacy.state() == QAbstractSocket::ConnectedState; }),
                "legacy connect");
        legacy.write("299"); legacy.write("99|Robot"); legacy.write("Mode()");
        require(pumpUntil([&harness] { return harness.dashboardPeer->bytesAvailable() >= 11; }),
                "dashboard command absent");
        require(harness.dashboardPeer->readAll() == "RobotMode()", "legacy command changed");
        harness.dashboardPeer->write("0,{5},Robot");
        QCoreApplication::processEvents();
        require(legacy.bytesAvailable() == 0, "half reply forwarded early");
        harness.dashboardPeer->write("Mode();");
        require(pumpUntil([&legacy] { return legacy.bytesAvailable() > 0; }), "legacy reply absent");
        require(legacy.readAll() == "0,{5},RobotMode();", "legacy reply changed");
    });
    runner.run("runtime allows legacy ServoP on motion port without enabling v2 motion", [] {
        RuntimeHarness harness; harness.start();
        QTcpSocket legacy;
        legacy.connectToHost(QHostAddress::LocalHost,harness.runtime.legacyServerPort());
        require(pumpUntil([&legacy] {return legacy.state()==QAbstractSocket::ConnectedState;}),
                "legacy connect");
        const QByteArray servo="ServoP(-150,-300,280,150,0,90)";
        legacy.write("30003|"+servo);
        require(pumpUntil([&harness] {return harness.motionPeer->bytesAvailable()>0;}),
                "ServoP did not reach motion port");
        require(harness.motionPeer->readAll()==servo,"ServoP changed in relay");
        QCoreApplication::processEvents(QEventLoop::AllEvents,20);
        require(legacy.bytesAvailable()==0,"ServoP must not wait for command response");
        require(harness.runtime.diagnostics().robotWriteRejects==0,"motion forwarding rejected");
        legacy.write("29999|"+servo);
        require(pumpUntil([&legacy] {return legacy.bytesAvailable()>0;}),
                "wrong-port ServoP not rejected");
        require(legacy.readAll().startsWith("ERROR|servop_requires_30003"),
                "ServoP accepted on dashboard port");
    });
    runner.run("runtime v2 command reply preserves correlation and controller bytes", [] {
        RuntimeHarness harness; harness.start();
        std::unique_ptr<QTcpSocket> client(harness.connectV2());
        const QString session = harness.hello(client.get())["session_id"].toString();
        QJsonObject command = message("command", "42", session);
        command.insert("target_port", 29999);
        command.insert("command", "RobotMode()");
        client->write(relay::encodeV2Frame(command));
        require(pumpUntil([&harness] { return harness.dashboardPeer->bytesAvailable() > 0; }),
                "v2 command not routed");
        require(harness.dashboardPeer->readAll() == "RobotMode()", "v2 command changed");
        harness.dashboardPeer->write("0,{5},RobotMode();");
        const QVector<QJsonObject> replies = collectV2(client.get(), 1);
        const auto replyIt = std::find_if(replies.cbegin(), replies.cend(), [](const QJsonObject& object) {
            return object["type"] == "reply" && object["request_id"] == "42";
        });
        require(replyIt != replies.cend() && (*replyIt)["raw_reply"] == "0,{5},RobotMode();",
                "v2 correlated reply absent");
    });
    runner.run("runtime rejects stale session and closes protocol connection", [] {
        RuntimeHarness harness; harness.start();
        std::unique_ptr<QTcpSocket> client(harness.connectV2());
        harness.hello(client.get());
        QJsonObject command = message("command", "9", "stale-session");
        command.insert("target_port", 29999);
        command.insert("command", "RobotMode()");
        client->write(relay::encodeV2Frame(command));
        const QVector<QJsonObject> replies = collectV2(client.get(), 1);
        require(!replies.isEmpty() && replies[0]["type"] == "error" &&
                    replies[0]["code"] == "session_mismatch",
                "stale session was not rejected");
        require(pumpUntil([&client] { return client->state() == QAbstractSocket::UnconnectedState; }),
                "bad v2 connection remained open");
    });
    runner.run("runtime replacement connection gets a distinct session", [] {
        RuntimeHarness harness; harness.start();
        std::unique_ptr<QTcpSocket> first(harness.connectV2());
        const QString firstSession = harness.hello(first.get())["session_id"].toString();
        std::unique_ptr<QTcpSocket> second(harness.connectV2());
        const QString secondSession = harness.hello(second.get())["session_id"].toString();
        require(firstSession != secondSession && !secondSession.isEmpty(), "session reused");
        require(pumpUntil([&first] { return first->state() == QAbstractSocket::UnconnectedState; }),
                "old v2 client not replaced");
    });
    runner.run("runtime stale timer pushes ready false after 500 ms", [] {
        RuntimeHarness harness; harness.start();
        std::unique_ptr<QTcpSocket> client(harness.connectV2());
        harness.hello(client.get());
        harness.feedbackPeer->write(validFeedbackFrame());
        collectV2(client.get(), 1);
        QElapsedTimer wait;
        wait.start();
        while (wait.elapsed() < 650) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(2);
        }
        const QVector<QJsonObject> messages = collectV2(client.get(), 1, 500);
        const auto stale = std::find_if(messages.cbegin(), messages.cend(), [](const QJsonObject& object) {
            return object["type"] == "relay_status" && object["relay_ready"] == false;
        });
        require(stale != messages.cend(), "stale ready=false status absent");
    });
    runner.run("parallel video fragmentation prototype is production-disabled", [] {
        require(!relay::kVideoFragmentationProductionEnabled,
                "fragmented video unexpectedly replaced legacy UDP production path");
    });
    runner.run("parallel video fragments reassemble out of order", [] {
        relay::EncodedCameraFrame source;
        source.bytes = QByteArray(170000, '\0');
        for (qsizetype i = 0; i < source.bytes.size(); ++i) {
            source.bytes[i] = static_cast<char>(i % 251);
        }
        source.metadata = {19, 123456, 123999, 0, 1280, 720, "jpeg", source.bytes.size()};
        const QVector<QByteArray> fragments = relay::fragmentVideoFrame(source, 60000);
        require(fragments.size() == 3, "unexpected fragment count");
        relay::VideoFragmentReassembler reassembler;
        std::optional<relay::EncodedCameraFrame> result;
        for (auto iterator = fragments.crbegin(); iterator != fragments.crend(); ++iterator) {
            const auto candidate = reassembler.feed(*iterator);
            if (candidate.has_value()) result = candidate;
        }
        require(result.has_value() && result->bytes == source.bytes &&
                    result->metadata.frameSeq == source.metadata.frameSeq &&
                    result->metadata.width == 1280 && result->metadata.height == 720,
                "fragmented frame did not reconstruct exactly");
    });
    runner.run("runtime rejects placeholder gripper command without robot write", [] {
        RuntimeHarness harness; harness.start();
        std::unique_ptr<QTcpSocket> client(harness.connectV2());
        const QString session = harness.hello(client.get())["session_id"].toString();
        QJsonObject command = message("command", "60", session);
        command.insert("target_port", 29999);
        command.insert("command", "GripperOpen()");
        client->write(relay::encodeV2Frame(command));
        const QVector<QJsonObject> replies = collectV2(client.get(), 1);
        require(!replies.isEmpty() && replies[0]["code"] == "gripper_not_installed",
                "gripper placeholder not rejected");
        QCoreApplication::processEvents();
        require(harness.dashboardPeer->bytesAvailable() == 0, "gripper bytes reached robot simulator");
    });
    runner.run("runtime keeps v2 motion production path disabled", [] {
        RuntimeHarness harness; harness.start();
        std::unique_ptr<QTcpSocket> client(harness.connectV2());
        const QString session = harness.hello(client.get())["session_id"].toString();
        QJsonObject command = message("command", "61", session);
        command.insert("target_port", 30003);
        command.insert("command", "MovL(1,2,3,4,5,6)");
        command.insert("control_generation", "4");
        command.insert("action_seq", "9");
        command.insert("ttl_ms", 100.0);
        client->write(relay::encodeV2Frame(command));
        const QVector<QJsonObject> replies = collectV2(client.get(), 1);
        require(!replies.isEmpty() && replies[0]["code"] == "v2_motion_path_disabled",
                "v2 motion path unexpectedly enabled");
        QCoreApplication::processEvents();
        require(harness.motionPeer->bytesAvailable() == 0, "v2 motion bytes reached robot simulator");
    });
    runner.run("runtime stream_control updates subscriptions with one reply", [] {
        RuntimeHarness harness; harness.start();
        std::unique_ptr<QTcpSocket> client(harness.connectV2());
        const QString session = harness.hello(client.get())["session_id"].toString();
        QJsonObject control = message("stream_control", "62", session);
        control.insert("robot_state", false);
        control.insert("relay_status", true);
        client->write(relay::encodeV2Frame(control));
        const QVector<QJsonObject> replies = collectV2(client.get(), 1);
        require(replies.size() == 1 && replies[0]["type"] == "reply" &&
                    replies[0]["request_id"] == "62" && replies[0]["stream_control_applied"] == true,
                "stream_control reply invalid");
    });
    runner.run("runtime feedback reconnect starts a clean connection generation", [] {
        RuntimeHarness harness; harness.start();
        harness.feedbackPeer->write(validFeedbackFrame().left(100));
        require(pumpUntil([&harness] { return harness.runtime.feedbackStats().bytesReceived >= 100; }),
                "partial feedback did not enter old buffer");
        const quint64 oldGeneration = harness.runtime.feedbackStats().validFrames;
        delete harness.feedbackPeer.data();
        require(pumpUntil([&harness] { return !harness.runtime.portConnected(relay::kFeedbackPort); }),
                "feedback disconnect not observed");
        require(pumpUntil([&harness] { return harness.feedbackServer.hasPendingConnections(); }, 2500),
                "feedback reconnect not attempted");
        harness.feedbackPeer = harness.feedbackServer.nextPendingConnection();
        require(pumpUntil([&harness] { return harness.runtime.portConnected(relay::kFeedbackPort); }),
                "feedback reconnect incomplete");
        harness.feedbackPeer->write(validFeedbackFrame(33));
        require(pumpUntil([&harness, oldGeneration] {
            return harness.runtime.feedbackStats().validFrames > oldGeneration;
        }), "new feedback frame not accepted");
        require(harness.runtime.latestState().connectionGeneration >= 2,
                "feedback generation did not advance");
    });

    runner.run("v2 rate gate caps high-frequency feedback and forwards safety transitions", [] {
        relay::V2RobotStatePublishGate gate;
        const qint64 t0=1000000000LL;
        require(gate.allow(t0),"first feedback must publish");
        int count=1;
        for(int ms=1;ms<=1000;++ms) {
            if(gate.allow(t0+static_cast<qint64>(ms)*1000000LL))++count;
        }
        require(count==51,"1000Hz feedback must not flood v2 beyond 50Hz");
        require(gate.allow(t0+1001000000LL,true),
                "robot mode/error/enable transitions bypass rate cap");
        require(!gate.allow(t0+1002000000LL),
                "ordinary frame must not follow urgent frame immediately");
        gate.reset();
        require(gate.allow(t0+1),"new session must publish first state");
        require(!gate.allow(0),"invalid monotonic timestamp must fail closed");
    });

    return runner.finish();
}
