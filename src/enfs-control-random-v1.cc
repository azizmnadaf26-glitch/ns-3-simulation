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
static uint32_t g_congestionThreshold = 15;
static bool g_congestionActive = false;
static const bool g_enfsEnabled = false;
static uint64_t g_feedbackEvents = 0;
static uint64_t g_feedbackMessagesSent = 0;
static uint64_t g_feedbackMessagesReceived = 0;

static Ptr<Socket> g_feedbackSocket;
static std::vector<Ipv4Address> g_senderAddresses;
static const uint16_t g_feedbackPort = 9000;

static void SendFeedback(uint8_t message);

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
          m_packetsSent(0)
    {
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

  private:
    void StartApplication() override
    {
        // TCP carries application data.
        m_dataSocket = Socket::CreateSocket(
            GetNode(), TcpSocketFactory::GetTypeId());

        m_dataSocket->SetConnectCallback(
            MakeCallback(&EnfsPacedTcpSender::ConnectionSucceeded, this),
            MakeCallback(&EnfsPacedTcpSender::ConnectionFailed, this));

        m_dataSocket->Connect(
            InetSocketAddress(m_destination, m_dataPort));

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
    }

    void StopApplication() override
    {
        // Stop TCP data generation, retaining the UDP listener.
        Simulator::Cancel(m_sendEvent);

        if (m_dataSocket)
        {
            m_dataSocket->Close();
            m_dataSocket = nullptr;
        }

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
            if (packet->GetSize() < 1)
            {
                continue;
            }

            uint8_t message = 255;
            packet->CopyData(&message, 1);

            if (message == 1 && !m_congested)
            {
                m_congested = true;
                m_currentRate = std::max(
                    m_minRate, m_currentRate / 2);

                ++g_feedbackMessagesReceived;

                std::cout << "[ENFS] Sender " << m_senderId
                          << " received CONGESTION feedback at "
                          << Simulator::Now().GetSeconds()
                          << " s; offered rate now "
                          << m_currentRate / 1000000.0
                          << " Mbps\n";
            }
            else if (message == 0 && m_congested)
            {
                m_congested = false;

                // Additive rate recovery after congestion clears.
                m_currentRate = std::min(
                    m_maxRate, m_currentRate + 250000);

                ++g_feedbackMessagesReceived;

                std::cout << "[ENFS] Sender " << m_senderId
                          << " received CLEAR feedback at "
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
};

static void
SendFeedback(uint8_t message)
{
    // Matched control: suppress ENFS feedback and sender rate changes.
    if (!g_enfsEnabled)
    {
        return;
    }

    if (!g_feedbackSocket)
    {
        return;
    }

    for (const auto &senderAddress : g_senderAddresses)
    {
        Ptr<Packet> packet = Create<Packet>(&message, 1);

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
QueueOccupancyChanged(uint32_t, uint32_t newValue)
{
    g_maxQueuePackets = std::max(g_maxQueuePackets, newValue);
    ++g_queueSamples;

    if (newValue >= g_congestionThreshold && !g_congestionActive)
    {
        g_congestionActive = true;
        ++g_feedbackEvents;

        std::cout << "\n[ENFS] Congestion detected at "
                  << Simulator::Now().GetSeconds()
                  << " s; queue = " << newValue
                  << " packets; event #" << g_feedbackEvents
                  << "\n";

        SendFeedback(1);
    }
    else if (newValue <= 10 && g_congestionActive)
    {
        g_congestionActive = false;

        std::cout << "\n[ENFS] Congestion cleared at "
                  << Simulator::Now().GetSeconds()
                  << " s; queue = " << newValue
                  << " packets\n";

        SendFeedback(0);
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
    cmd.Parse(argc, argv);

    NS_ABORT_MSG_IF(jitterMin <= 0.0 || jitterMax < jitterMin,
                    "Invalid jitter bounds");

    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(run);
    NS_ABORT_MSG_IF(startOffsetMs < 0.0,
                      "startOffsetMs must be non-negative");

    if (numberOfSenders == 0 || simulationTime <= 2.0 || bottleneckMbps <= 0.0)
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

        // Match ENFS TCP connection start times.
        const double trafficStart =
            1.0 + i * startOffsetMs / 1000.0;

        sender->SetStartTime(Seconds(trafficStart));
        sender->SetStopTime(Seconds(simulationTime));
    }

    FlowMonitorHelper flowHelper;
    Ptr<FlowMonitor> monitor = flowHelper.InstallAll();

    std::cout << "\nMatched Control - Paced TCP Without ENFS Feedback\n"
              << "Senders: " << numberOfSenders << "\n"
              << "Startup offset: " << startOffsetMs
              << " ms per sender index\n"
              << "TCP: NewReno; data port: " << dataPort << "\n"
              << "ENFS feedback: UDP port " << g_feedbackPort << "\n"
              << "Bottleneck: " << bottleneckMbps << " Mbps; queue limit: 20 packets\n"
              << "Congestion threshold: " << g_congestionThreshold
              << " packets; clear threshold: 10 packets\n"
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

    std::cout << "===== ENFS AGGREGATE RESULTS =====\n"
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

    monitor->SerializeToXmlFile(
        "enfs-feedback-v2-flowmon.xml", true, true);

    Simulator::Destroy();
    return 0;
}
