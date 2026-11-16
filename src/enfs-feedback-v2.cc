#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

#include "ns3/core-module.h"
#include "ns3/random-variable-stream.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/tcp-congestion-ops.h"

using namespace ns3;

// ENFS prototype state
static uint32_t g_maxQueuePackets = 0;
static uint64_t g_queueSamples = 0;
static uint32_t g_currentQueuePackets = 0;
static uint32_t g_congestionThreshold = 15;
static Time g_feedbackGenerationDeadline = Seconds(12.0);
static bool g_congestionActive = false;

// ENFS v2: temporal hysteresis configuration and pending transitions.
static Time g_persistenceInterval = MilliSeconds(50);
static bool g_congestionTransitionPending = false;
static bool g_clearTransitionPending = false;
static EventId g_pendingCongestionEvent;
static EventId g_pendingClearEvent;
static Time g_congestionCandidateStart;
static Time g_clearCandidateStart;

static uint32_t g_congestionCandidatesStarted = 0;
static uint32_t g_clearCandidatesStarted = 0;
static uint32_t g_congestionCandidatesCancelled = 0;
static uint32_t g_clearCandidatesCancelled = 0;
static uint32_t g_congestionTransitionsConfirmed = 0;
static uint32_t g_clearTransitionsConfirmed = 0;

static uint64_t g_feedbackEvents = 0;
static uint64_t g_feedbackMessagesSent = 0;
static uint64_t g_feedbackMessagesReceived = 0;
static uint32_t g_feedbackSequence = 0;
static uint64_t g_recoveryIncrementBps = 100000;

// V4: rate-limit feedback while retaining the latest queue state.
static const Time g_feedbackCooldown = MilliSeconds(100);
static Time g_lastFeedbackTime = Seconds(-1);
static bool g_feedbackPending = false;
static uint8_t g_pendingFeedbackMessage = 0;
static EventId g_pendingFeedbackEvent;

static Ptr<Socket> g_feedbackSocket;
static std::vector<Ipv4Address> g_senderAddresses;
static const uint16_t g_feedbackPort = 9000;

static void SendFeedback(uint8_t message);
static void FlushPendingFeedback();

class EnfsPacedTcpSender : public Application
{
  public:
    EnfsPacedTcpSender()
        : m_dataSocket(nullptr),
          m_feedbackSocket(nullptr),
          m_connected(false),
          m_congested(false),
          m_packetSize(1448),
          m_initialRate(3000000),
          m_currentRate(3000000),
          m_maxRate(3000000),
          m_minRate(500000),
          m_packetsSent(0),
          m_lastFeedbackSequence(0)
    {
    }
void SetTrafficStartTime(Time startTime)
{
    m_trafficStartTime = startTime;
}
    void Setup(Ipv4Address destination, uint16_t dataPort,
               uint32_t senderId, double simulationEndTime)
    {
        m_destination = destination;
        m_dataPort = dataPort;
        m_senderId = senderId;
        m_simulationEndTime = simulationEndTime;
    }

    void SetPacingMultipliers(const std::vector<double>& multipliers)
    {
        m_pacingMultipliers = multipliers;
        m_pacingIndex = 0;
    }

    uint32_t GetSenderIdForDiagnostics() const { return m_senderId; }
    uint64_t GetFeedbackPacketsReceivedForDiagnostics() const { return m_feedbackPacketsReceived; }
    uint64_t GetFreshFeedbackAcceptedForDiagnostics() const { return m_freshFeedbackAccepted; }
    uint64_t GetStaleFeedbackRejectedForDiagnostics() const { return m_staleFeedbackRejected; }
    uint64_t GetStateChangesAppliedForDiagnostics() const { return m_stateChangesApplied; }

  private:
    void StartApplication() override
    {
        
        // UDP is used only for explicit ENFS control messages.
        m_feedbackSocket = Socket::CreateSocket(
            GetNode(), UdpSocketFactory::GetTypeId());

        if (m_feedbackSocket->Bind(
                InetSocketAddress(Ipv4Address::GetAny(),
                                  g_feedbackPort)) != 0)
        {
            NS_FATAL_ERROR("Could not bind ENFS feedback listener");
        }

        m_feedbackSocket->SetRecvCallback(
            MakeCallback(&EnfsPacedTcpSender::ReceiveFeedback, this));
        
        // Start TCP traffic at the configured staggered time.
        Simulator::Schedule(
            std::max(Seconds(0), m_trafficStartTime - Simulator::Now()),
            &EnfsPacedTcpSender::StartTcpConnection, this);
    
    }
    
    void StartTcpConnection()
    {
        if (!m_dataSocket)
        {
            m_dataSocket = Socket::CreateSocket(
                GetNode(), TcpSocketFactory::GetTypeId());

            m_dataSocket->SetConnectCallback(
                MakeCallback(&EnfsPacedTcpSender::ConnectionSucceeded, this),
                MakeCallback(&EnfsPacedTcpSender::ConnectionFailed, this));

            m_dataSocket->Connect(
                InetSocketAddress(m_destination, m_dataPort));
        }
    }

    void StopApplication() override
    {
        // Stop TCP data generation at the configured data stop time.
        Simulator::Cancel(m_sendEvent);

        if (m_dataSocket)
        {
            m_dataSocket->Close();
            m_dataSocket = nullptr;
        }

        // Keep the UDP feedback listener alive until simulation end.
        if (m_feedbackSocket && !m_feedbackCloseScheduled)
        {
            m_feedbackCloseScheduled = true;
            Time remaining =
                Seconds(m_simulationEndTime) - Simulator::Now();

            if (remaining <= Seconds(0))
            {
                StopFeedbackListener();
            }
            else
            {
                Simulator::Schedule(
                    remaining,
                    &EnfsPacedTcpSender::StopFeedbackListener,
                    this);
            }
        }
    }

    void StopFeedbackListener()
    {
        if (m_feedbackSocket)
        {
            m_feedbackSocket->SetRecvCallback(
                MakeNullCallback<void, Ptr<Socket>>());
            m_feedbackSocket->Close();
            m_feedbackSocket = nullptr;
        }
    }

    void ConnectionSucceeded(Ptr<Socket> socket)
    {
        m_connected = true;

        std::cout << "[Sender " << m_senderId
                  << "] TCP connected at "
                  << Simulator::Now().GetSeconds() << " s\n";

        ScheduleNextPacket();
    }

    void ConnectionFailed(Ptr<Socket>)
    {
        std::cout << "[Sender " << m_senderId
                  << "] TCP connection failed\n";
    }

    void ScheduleNextPacket()
    {
        if (!m_connected)
        {
            return;
        }

        NS_ABORT_MSG_IF(
            m_pacingIndex >= m_pacingMultipliers.size(),
            "Pacing multiplier sequence exhausted for sender "
                << m_senderId);

        const double jitter = m_pacingMultipliers[m_pacingIndex++];

        const double secondsPerPacket =
            (static_cast<double>(m_packetSize) * 8.0 /
             static_cast<double>(m_currentRate)) * jitter;

        m_sendEvent = Simulator::Schedule(
            Seconds(secondsPerPacket),
            &EnfsPacedTcpSender::SendOnePacket, this);
    }

    void SendOnePacket()
    {
        if (!m_connected || !m_dataSocket)
        {
            return;
        }

        Ptr<Packet> packet = Create<Packet>(m_packetSize);
        int result = m_dataSocket->Send(packet);

        if (result >= 0)
        {
            ++m_packetsSent;
        }

        ScheduleNextPacket();
    }

    void ReceiveFeedback(Ptr<Socket> socket)
    {
        Address from;

        while (Ptr<Packet> packet = socket->RecvFrom(from))
        {
            ++m_feedbackPacketsReceived;

            if (packet->GetSize() < 5)
            {
                continue;
            }

            uint8_t data[5] = {};
            packet->CopyData(data, 5);

            uint32_t sequence =
                (static_cast<uint32_t>(data[0]) << 24) |
                (static_cast<uint32_t>(data[1]) << 16) |
                (static_cast<uint32_t>(data[2]) << 8) |
                static_cast<uint32_t>(data[3]);

            uint8_t message = data[4];

            if (sequence <= m_lastFeedbackSequence)
            {
                ++m_staleFeedbackRejected;
                std::cout << "[ENFS] Sender " << m_senderId
                          << " ignored stale sequence "
                          << sequence << "\n";
                continue;
            }

            m_lastFeedbackSequence = sequence;
            ++g_feedbackMessagesReceived;
            ++m_freshFeedbackAccepted;

            if (message == 1 && !m_congested)
            {
                ++m_stateChangesApplied;
                m_congested = true;

                // Retain 75% of the current offered rate.
                m_currentRate = std::max(
                    m_minRate, m_currentRate * 3 / 4);

                std::cout << "[ENFS] Sender " << m_senderId
                          << " accepted CONGESTION sequence "
                          << sequence << " at "
                          << Simulator::Now().GetSeconds()
                          << " s; offered rate now "
                          << m_currentRate / 1000000.0
                          << " Mbps\n";
            }
            else if (message == 0 && m_congested)
            {
                ++m_stateChangesApplied;
                m_congested = false;

                // Gradual recovery: increase by 0.10 Mbps.
                m_currentRate = std::min(
                    m_maxRate, m_currentRate + g_recoveryIncrementBps);

                std::cout << "[ENFS] Sender " << m_senderId
                          << " accepted CLEAR sequence "
                          << sequence << " at "
                          << Simulator::Now().GetSeconds()
                          << " s; offered rate now "
                          << m_currentRate / 1000000.0
                          << " Mbps\n";
            }
        }
    }

    std::vector<double> m_pacingMultipliers;
    uint64_t m_pacingIndex{0};

    Ptr<Socket> m_dataSocket;
    Ptr<Socket> m_feedbackSocket;
    EventId m_sendEvent;
    Ipv4Address m_destination;
    double m_simulationEndTime{12.0};
      // Configured start time for TCP traffic.
    Time m_trafficStartTime{Seconds(1.0)};
    bool m_feedbackCloseScheduled{false};
    uint16_t m_dataPort{5000};
    uint32_t m_senderId{0};
    bool m_connected;
    bool m_congested;
    uint32_t m_packetSize;
    uint64_t m_initialRate;
    uint64_t m_currentRate;
    uint64_t m_maxRate;
    uint64_t m_minRate;
    uint64_t m_packetsSent;
    uint32_t m_lastFeedbackSequence;
    uint64_t m_feedbackPacketsReceived{0};
    uint64_t m_freshFeedbackAccepted{0};
    uint64_t m_staleFeedbackRejected{0};
    uint64_t m_stateChangesApplied{0};
};

static void
SendFeedback(uint8_t message)
{
    if (!g_feedbackSocket)
    {
        return;
    }

    // Keep only the newest requested queue state.
    g_feedbackPending = true;
    g_pendingFeedbackMessage = message;

    if (g_lastFeedbackTime >= Seconds(0))
    {
        Time elapsed = Simulator::Now() - g_lastFeedbackTime;
        if (elapsed < g_feedbackCooldown)
        {
            if (!g_pendingFeedbackEvent.IsPending())
            {
                g_pendingFeedbackEvent = Simulator::Schedule(
                    g_feedbackCooldown - elapsed,
                    &FlushPendingFeedback);
            }
            return;
        }
    }

    FlushPendingFeedback();
}

static void
FlushPendingFeedback()
{
    if (!g_feedbackSocket || !g_feedbackPending)
    {
        return;
    }

    uint8_t message = g_pendingFeedbackMessage;
    g_feedbackPending = false;

    // Cancel any outstanding cooldown callback before flushing.
    if (g_pendingFeedbackEvent.IsPending())
    {
        Simulator::Cancel(g_pendingFeedbackEvent);
    }

    g_lastFeedbackTime = Simulator::Now();

    ++g_feedbackSequence;

    uint8_t data[5] = {
        static_cast<uint8_t>((g_feedbackSequence >> 24) & 0xff),
        static_cast<uint8_t>((g_feedbackSequence >> 16) & 0xff),
        static_cast<uint8_t>((g_feedbackSequence >> 8) & 0xff),
        static_cast<uint8_t>(g_feedbackSequence & 0xff),
        message
    };

    for (const auto &senderAddress : g_senderAddresses)
    {
        Ptr<Packet> packet = Create<Packet>(data, sizeof(data));

        int result = g_feedbackSocket->SendTo(
            packet, 0,
            InetSocketAddress(senderAddress, g_feedbackPort));

        if (result >= 0)
        {
            ++g_feedbackMessagesSent;
        }

        std::cout << "[ENFS] Sent "
                  << (message == 1 ? "CONGESTION" : "CLEAR")
                  << " feedback to " << senderAddress
                  << " at " << Simulator::Now().GetSeconds()
                  << " s\n";
    }
}

static void
ConfirmCongestion()
{
    g_congestionTransitionPending = false;

    if (Simulator::Now() >= g_feedbackGenerationDeadline)
    {
        return;
    }

    // Recheck the queue when the persistence timer expires.
    if (!g_congestionActive &&
        g_currentQueuePackets >= g_congestionThreshold)
    {
        g_congestionActive = true;
        ++g_feedbackEvents;
        ++g_congestionTransitionsConfirmed;

        std::cout << "\n[ENFS v2] Congestion confirmed at "
                  << Simulator::Now().GetSeconds()
                  << " s; persistence = "
                  << (Simulator::Now() - g_congestionCandidateStart).GetMilliSeconds()
                  << " ms; queue = " << g_currentQueuePackets
                  << " packets; event #" << g_feedbackEvents
                  << "\n";

        SendFeedback(1);
    }
}

static void
ConfirmClear()
{
    g_clearTransitionPending = false;

    if (Simulator::Now() >= g_feedbackGenerationDeadline)
    {
        return;
    }

    // Recheck the queue when the persistence timer expires.
    if (g_congestionActive && g_currentQueuePackets <= 10)
    {
        g_congestionActive = false;
        ++g_clearTransitionsConfirmed;

        std::cout << "\n[ENFS v2] Congestion clear confirmed at "
                  << Simulator::Now().GetSeconds()
                  << " s; persistence = "
                  << (Simulator::Now() - g_clearCandidateStart).GetMilliSeconds()
                  << " ms; queue = " << g_currentQueuePackets
                  << " packets\n";

        SendFeedback(0);
    }
}

static void
QueueOccupancyChanged(uint32_t, uint32_t newValue)
{
    g_currentQueuePackets = newValue;
    g_maxQueuePackets = std::max(g_maxQueuePackets, newValue);
    ++g_queueSamples;

    if (Simulator::Now() >= g_feedbackGenerationDeadline)
    {
        return;
    }

    if (!g_congestionActive)
    {
        // Cancel a pending clear transition if congestion is not active.
        if (g_clearTransitionPending)
        {
            Simulator::Cancel(g_pendingClearEvent);
            ++g_clearCandidatesCancelled;
            g_clearTransitionPending = false;
        }

        if (newValue >= g_congestionThreshold)
        {
            if (!g_congestionTransitionPending)
            {
                g_congestionTransitionPending = true;
                g_congestionCandidateStart = Simulator::Now();
                ++g_congestionCandidatesStarted;
                std::cout << "[ENFS v2] Congestion candidate started at "
                          << Simulator::Now().GetSeconds()
                          << " s; queue = " << newValue << " packets\n";
                g_pendingCongestionEvent = Simulator::Schedule(
                    g_persistenceInterval,
                    &ConfirmCongestion);
            }
        }
        else if (g_congestionTransitionPending)
        {
            Simulator::Cancel(g_pendingCongestionEvent);
            ++g_congestionCandidatesCancelled;
            g_congestionTransitionPending = false;
        }
    }
    else
    {
        // Cancel a pending congestion transition while already congested.
        if (g_congestionTransitionPending)
        {
            Simulator::Cancel(g_pendingCongestionEvent);
            ++g_congestionCandidatesCancelled;
            g_congestionTransitionPending = false;
        }

        if (newValue <= 10)
        {
            if (!g_clearTransitionPending)
            {
                g_clearTransitionPending = true;
                g_clearCandidateStart = Simulator::Now();
                ++g_clearCandidatesStarted;
                std::cout << "[ENFS v2] Clear candidate started at "
                          << Simulator::Now().GetSeconds()
                          << " s; queue = " << newValue << " packets\n";
                g_pendingClearEvent = Simulator::Schedule(
                    g_persistenceInterval,
                    &ConfirmClear);
            }
        }
        else if (g_clearTransitionPending)
        {
            Simulator::Cancel(g_pendingClearEvent);
            ++g_clearCandidatesCancelled;
            g_clearTransitionPending = false;
        }
    }
}

int main(int argc, char *argv[])
{
    uint32_t numberOfSenders = 3;
    double simulationTime = 12.0;
    double bottleneckMbps = 5.0;
    double startOffsetMs = 0.0;
    uint32_t seed = 101;
    uint64_t run = 1;
    double jitterMin = 0.90;
    double jitterMax = 1.10;
    double recoveryMbps = 0.10;
    double persistenceMs = 50.0;

    CommandLine cmd(__FILE__);
    cmd.AddValue("numberOfSenders", "Number of TCP senders",
                 numberOfSenders);
    cmd.AddValue("simulationTime", "Simulation duration in seconds",
                 simulationTime);
    cmd.AddValue("bottleneckMbps", "Bottleneck capacity in Mbps",
                 bottleneckMbps);
    cmd.AddValue("startOffsetMs",
                 "Per-sender startup offset in milliseconds",
                 startOffsetMs);
    cmd.AddValue("seed", "Random seed", seed);
    cmd.AddValue("run", "Random run number", run);
    cmd.AddValue("jitterMin", "Minimum pacing multiplier", jitterMin);
    cmd.AddValue("jitterMax", "Maximum pacing multiplier", jitterMax);
    cmd.AddValue("recoveryMbps", "Rate increase per accepted CLEAR message in Mbps",
                 recoveryMbps);
    cmd.AddValue("persistenceMs",
                 "Queue-state persistence interval in milliseconds",
                 persistenceMs);
    cmd.Parse(argc, argv);

    NS_ABORT_MSG_IF(jitterMin <= 0.0 || jitterMax < jitterMin,
                    "Invalid jitter bounds");

    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(run);
    g_feedbackGenerationDeadline = Seconds(simulationTime);

    NS_ABORT_MSG_IF(persistenceMs <= 0.0,
                    "persistenceMs must be positive");
    g_persistenceInterval = MilliSeconds(persistenceMs);

    NS_ABORT_MSG_IF(startOffsetMs < 0.0,
                      "startOffsetMs must be non-negative");
    g_recoveryIncrementBps = static_cast<uint64_t>(recoveryMbps * 1000000.0);

    if (numberOfSenders == 0 || simulationTime <= 2.0 || bottleneckMbps <= 0.0 || recoveryMbps <= 0.0)
    {
        NS_FATAL_ERROR(
            "Use at least one sender and simulationTime > 2 seconds");
    }

    Config::SetDefault(
        "ns3::TcpL4Protocol::SocketType",
        TypeIdValue(TcpNewReno::GetTypeId()));

    NodeContainer senders;
    senders.Create(numberOfSenders);

    Ptr<Node> router = CreateObject<Node>();
    Ptr<Node> receiver = CreateObject<Node>();

    InternetStackHelper internet;
    internet.Install(senders);
    internet.Install(router);
    internet.Install(receiver);

    PointToPointHelper accessLink;
    accessLink.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    accessLink.SetChannelAttribute("Delay", StringValue("1ms"));

    PointToPointHelper bottleneck;
    std::ostringstream bottleneckRate;
    bottleneckRate << bottleneckMbps << "Mbps";
    bottleneck.SetDeviceAttribute("DataRate", StringValue(bottleneckRate.str()));
    bottleneck.SetChannelAttribute("Delay", StringValue("10ms"));
    bottleneck.SetQueue(
        "ns3::DropTailQueue<Packet>",
        "MaxSize", QueueSizeValue(QueueSize("20p")));

    Ipv4AddressHelper address;
    std::vector<Ipv4InterfaceContainer> accessInterfaces;

    for (uint32_t i = 0; i < numberOfSenders; ++i)
    {
        NodeContainer pair(senders.Get(i), router);
        NetDeviceContainer devices = accessLink.Install(pair);

        std::ostringstream subnet;
        subnet << "10.1." << i + 1 << ".0";

        address.SetBase(subnet.str().c_str(), "255.255.255.0");
        Ipv4InterfaceContainer interfaces = address.Assign(devices);
        accessInterfaces.push_back(interfaces);

        g_senderAddresses.push_back(interfaces.GetAddress(0));
    }

    NodeContainer finalPair(router, receiver);
    NetDeviceContainer finalDevices = bottleneck.Install(finalPair);

    Ptr<PointToPointNetDevice> bottleneckDevice =
        DynamicCast<PointToPointNetDevice>(finalDevices.Get(0));

    NS_ABORT_MSG_IF(!bottleneckDevice, "Bottleneck device not found");

    Ptr<Queue<Packet>> bottleneckQueue = bottleneckDevice->GetQueue();

    NS_ABORT_MSG_IF(!bottleneckQueue, "Bottleneck queue not found");

    bool connected = bottleneckQueue->TraceConnectWithoutContext(
        "PacketsInQueue", MakeCallback(&QueueOccupancyChanged));

    NS_ABORT_MSG_IF(!connected, "Failed to connect queue trace");

    address.SetBase("10.2.0.0", "255.255.255.0");
    Ipv4InterfaceContainer receiverInterfaces =
        address.Assign(finalDevices);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // Router-originated UDP control channel for ENFS.
    g_feedbackSocket = Socket::CreateSocket(
        router, UdpSocketFactory::GetTypeId());

    NS_ABORT_MSG_IF(
        g_feedbackSocket->Bind() != 0,
        "Could not bind router ENFS feedback socket");

    const uint16_t dataPort = 5000;

    PacketSinkHelper sinkHelper(
        "ns3::TcpSocketFactory",
        InetSocketAddress(Ipv4Address::GetAny(), dataPort));

    ApplicationContainer sink = sinkHelper.Install(receiver);
    sink.Start(Seconds(0.0));
    const double cleanupTime = simulationTime + 0.2;
    sink.Stop(Seconds(cleanupTime));

    // Pre-generate matched pacing jitter sequences.
    // Each sender receives a dedicated deterministic RNG stream.
    constexpr uint64_t pacingSequenceCapacity = 20000;

    std::vector<std::vector<double>> pacingSequences(numberOfSenders);

    for (uint32_t i = 0; i < numberOfSenders; ++i)
    {
        Ptr<UniformRandomVariable> pacingRng =
            CreateObject<UniformRandomVariable>();

        pacingRng->SetStream(i);

        pacingSequences[i].reserve(pacingSequenceCapacity);

        for (uint64_t j = 0; j < pacingSequenceCapacity; ++j)
        {
            pacingSequences[i].push_back(
                pacingRng->GetValue(jitterMin, jitterMax));
        }
    }

    for (uint32_t i = 0; i < numberOfSenders; ++i)
    {
        Ptr<EnfsPacedTcpSender> sender =
            CreateObject<EnfsPacedTcpSender>();

        sender->Setup(receiverInterfaces.GetAddress(1),
                      dataPort, i + 1, cleanupTime);

        sender->SetPacingMultipliers(pacingSequences[i]);

        senders.Get(i)->AddApplication(sender);

        const double trafficStart =
            1.0 + i * startOffsetMs / 1000.0;

        sender->SetTrafficStartTime(Seconds(trafficStart));

// Start every application early so its UDP listener is ready.
        sender->SetStartTime(Seconds(0.5));
        sender->SetStopTime(Seconds(simulationTime));
     }

    FlowMonitorHelper flowHelper;
    Ptr<FlowMonitor> monitor = flowHelper.InstallAll();

    std::cout << "\nENFS Explicit Feedback Prototype v3 - Sequenced Feedback\n"
              << "Senders: " << numberOfSenders << "\n"
              << "Startup offset: " << startOffsetMs
              << " ms per sender index\n"
              << "TCP: NewReno; data port: " << dataPort << "\n"
              << "ENFS feedback: UDP port " << g_feedbackPort << "\n"
              << "Bottleneck: " << bottleneckMbps << " Mbps; queue limit: 20 packets\n"
              << "Congestion threshold: " << g_congestionThreshold
              << " packets; clear threshold: 10 packets\n"
              << "Persistence interval: "
              << g_persistenceInterval.GetMilliSeconds()
              << " ms\n"
              << "Simulation duration: " << simulationTime
              << " seconds\n\n";

    Simulator::Stop(Seconds(cleanupTime));
    Simulator::Run();

    monitor->CheckForLostPackets();

    Ptr<Ipv4FlowClassifier> classifier =
        DynamicCast<Ipv4FlowClassifier>(flowHelper.GetClassifier());

    auto stats = monitor->GetFlowStats();

    uint64_t totalTxPackets = 0;
    uint64_t totalRxPackets = 0;
    uint64_t totalRxBytes = 0;
    uint64_t totalLostPackets = 0;
    double totalDelaySeconds = 0.0;
    double firstRxTime = simulationTime;
    double lastRxTime = 0.0;

    std::cout << std::fixed << std::setprecision(3);

    for (const auto &entry : stats)
    {
        auto tuple = classifier->FindFlow(entry.first);

        if (tuple.destinationPort != dataPort)
        {
            continue;
        }

        const auto &s = entry.second;

        totalTxPackets += s.txPackets;
        totalRxPackets += s.rxPackets;
        totalRxBytes += s.rxBytes;
        totalLostPackets += s.lostPackets;
        totalDelaySeconds += s.delaySum.GetSeconds();

        if (s.rxPackets > 0)
        {
            firstRxTime = std::min(
                firstRxTime, s.timeFirstRxPacket.GetSeconds());

            lastRxTime = std::max(
                lastRxTime, s.timeLastRxPacket.GetSeconds());
        }

        double flowThroughput = 0.0;
        if (s.rxPackets > 0 &&
            s.timeLastRxPacket > s.timeFirstRxPacket)
        {
            double duration =
                (s.timeLastRxPacket -
                 s.timeFirstRxPacket).GetSeconds();

            flowThroughput = s.rxBytes * 8.0 / duration / 1e6;
        }

        double flowDelay = 0.0;
        if (s.rxPackets > 0)
        {
            flowDelay =
                s.delaySum.GetSeconds() * 1000.0 / s.rxPackets;
        }

        std::cout << "Flow " << entry.first << ": "
                  << tuple.sourceAddress << " -> "
                  << tuple.destinationAddress << "\n"
                  << "  TX packets: " << s.txPackets
                  << ", RX packets: " << s.rxPackets
                  << ", lost: " << s.lostPackets << "\n"
                  << "  Throughput: " << flowThroughput << " Mbps\n"
                  << "  Mean packet delay: " << flowDelay
                  << " ms\n\n";
    }

    double lossRatio = 0.0;
    if (totalTxPackets > 0)
    {
        lossRatio = static_cast<double>(totalLostPackets) /
                    totalTxPackets;
    }

    double aggregateThroughput = 0.0;
    if (totalRxPackets > 0 && lastRxTime > firstRxTime)
    {
        aggregateThroughput =
            totalRxBytes * 8.0 / (lastRxTime - firstRxTime) / 1e6;
    }

    double meanDelayMs = 0.0;
    if (totalRxPackets > 0)
    {
        meanDelayMs =
            totalDelaySeconds * 1000.0 / totalRxPackets;
    }

    std::cout << "\\n===== PER-SENDER FEEDBACK DIAGNOSTICS =====\\n";
    for (uint32_t i = 0; i < senders.GetN(); ++i)
    {
        Ptr<EnfsPacedTcpSender> sender =
            DynamicCast<EnfsPacedTcpSender>(
                senders.Get(i)->GetApplication(0));
        if (sender)
        {
            std::cout << "Sender " << sender->GetSenderIdForDiagnostics()
                      << ": received=" << sender->GetFeedbackPacketsReceivedForDiagnostics()
                      << ", fresh=" << sender->GetFreshFeedbackAcceptedForDiagnostics()
                      << ", stale=" << sender->GetStaleFeedbackRejectedForDiagnostics()
                      << ", state_changes=" << sender->GetStateChangesAppliedForDiagnostics()
                      << "\\n";
        }
    }

    std::cout << "===== ENFS AGGREGATE RESULTS =====\\n"
              << "Transmitted packets: " << totalTxPackets << "\n"
              << "Received packets: " << totalRxPackets << "\n"
              << "Lost packets (FlowMonitor): "
              << totalLostPackets << "\n"
              << "Loss ratio: " << lossRatio << "\n"
              << "Aggregate throughput estimate: "
              << aggregateThroughput << " Mbps\n"
              << "Mean packet delay: " << meanDelayMs << " ms\n"
              << "Maximum queue occupancy: "
              << g_maxQueuePackets << " packets\n"
              << "Queue occupancy change events: "
              << g_queueSamples << "\n"
              << "Congestion episodes: " << g_feedbackEvents << "\n"
              << "Feedback messages sent: "
              << g_feedbackMessagesSent << "\n"
              << "Feedback messages received: "
              << g_feedbackMessagesReceived << "\n";

    std::cout << "\n===== ENFS V2 TEMPORAL HYSTERESIS DIAGNOSTICS =====\n"
              << "Congestion candidates started: "
              << g_congestionCandidatesStarted << "\n"
              << "Congestion candidates cancelled: "
              << g_congestionCandidatesCancelled << "\n"
              << "Congestion transitions confirmed: "
              << g_congestionTransitionsConfirmed << "\n"
              << "Clear candidates started: "
              << g_clearCandidatesStarted << "\n"
              << "Clear candidates cancelled: "
              << g_clearCandidatesCancelled << "\n"
              << "Clear transitions confirmed: "
              << g_clearTransitionsConfirmed << "\n";

    monitor->SerializeToXmlFile(
        "enfs-feedback-v2-flowmon.xml", true, true);

    Simulator::Destroy();
    return 0;
}
