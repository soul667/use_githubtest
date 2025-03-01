#include "ns3/dv-routing-protocol.h"
#include "ns3/double.h"
#include "ns3/inet-socket-address.h"
#include "ns3/ipv4-header.h"
#include "ns3/ipv4-packet-info-tag.h"
#include "ns3/ipv4-route.h"
#include "ns3/log.h"
#include "ns3/random-variable-stream.h"
#include "ns3/simulator.h"
#include "ns3/socket-factory.h"
#include "ns3/test-result.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/uinteger.h"
#include <ctime>
#include <map>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("DVRoutingProtocol");
NS_OBJECT_ENSURE_REGISTERED(DVRoutingProtocol);

#define DV_MAX_SEQUENCE_NUMBER 0xFFFF
#define DV_PORT_NUMBER 698

TypeId
DVRoutingProtocol::GetTypeId(void) {
  static TypeId tid = TypeId("DVRoutingProtocol")
                          .SetParent<PennRoutingProtocol>()
                          .AddConstructor<DVRoutingProtocol>()
                          .AddAttribute("DVPort",
                                        "Listening port for DV packets",
                                        UintegerValue(5000),
                                        MakeUintegerAccessor(&DVRoutingProtocol::m_dvPort),
                                        MakeUintegerChecker<uint16_t>())
                          .AddAttribute("PingTimeout",
                                        "Timeout value for PING_REQ in milliseconds",
                                        TimeValue(MilliSeconds(2000)),
                                        MakeTimeAccessor(&DVRoutingProtocol::m_pingTimeout),
                                        MakeTimeChecker())
                          .AddAttribute("MaxTTL",
                                        "Maximum TTL value for DV packets",
                                        UintegerValue(16),
                                        MakeUintegerAccessor(&DVRoutingProtocol::m_maxTTL),
                                        MakeUintegerChecker<uint8_t>())
                          .AddAttribute("AuditInterval",
                                        "Interval for auditing neighbors in milliseconds",
                                        TimeValue(MilliSeconds(100)),
                                        MakeTimeAccessor(&DVRoutingProtocol::m_auditInterval),
                                        MakeTimeChecker())
                          .AddAttribute("HelloInterval",
                                        "Interval value for HELLO in milliseconds",
                                        TimeValue(MilliSeconds(100)),
                                        MakeTimeAccessor(&DVRoutingProtocol::m_helloInterval),
                                        MakeTimeChecker())
                          .AddAttribute("HelloTimeout",
                                        "Timeout value for HELLO in milliseconds",
                                        TimeValue(MilliSeconds(500)),
                                        MakeTimeAccessor(&DVRoutingProtocol::m_helloTimeout),
                                        MakeTimeChecker())
                          .AddAttribute("TriggerInterval",
                                        "Trigger value for updates in milliseconds",
                                        TimeValue(MilliSeconds(20)),
                                        MakeTimeAccessor(&DVRoutingProtocol::m_triggerInterval),
                                        MakeTimeChecker());
  return tid;
}

DVRoutingProtocol::DVRoutingProtocol()
    : m_auditPingsTimer(Timer::CANCEL_ON_DESTROY) {
  m_currentSequenceNumber = 0;
  // Setup static routing
  m_staticRouting = Create<Ipv4StaticRouting>();
}

DVRoutingProtocol::~DVRoutingProtocol() {}

void DVRoutingProtocol::DoDispose() {
  if (m_recvSocket) {
    m_recvSocket->Close();
    m_recvSocket = 0;
  }

  // Close sockets
  for (std::map<Ptr<Socket>, Ipv4InterfaceAddress>::iterator iter = m_socketAddresses.begin();
      iter != m_socketAddresses.end(); iter++) {
    iter->first->Close();
  }
  m_socketAddresses.clear();

  // Clear static routing
  m_staticRouting = 0;

  // Cancel timers
  m_auditPingsTimer.Cancel();
  m_pingTracker.clear();
  m_auditTimer.Cancel();
  m_helloTimer.Cancel();
  m_triggerTimer.Cancel();

  PennRoutingProtocol::DoDispose();
}

void DVRoutingProtocol::SetMainInterface(uint32_t mainInterface) {
  m_mainAddress = m_ipv4->GetAddress(mainInterface, 0).GetLocal();
}

void DVRoutingProtocol::SetNodeAddressMap(std::map<uint32_t, Ipv4Address> nodeAddressMap) {
  m_nodeAddressMap = nodeAddressMap;
}

void DVRoutingProtocol::SetAddressNodeMap(std::map<Ipv4Address, uint32_t> addressNodeMap) {
  m_addressNodeMap = addressNodeMap;
}

Ipv4Address
DVRoutingProtocol::ResolveNodeIpAddress(uint32_t nodeNumber) {
  std::map<uint32_t, Ipv4Address>::iterator iter = m_nodeAddressMap.find(nodeNumber);
  if (iter != m_nodeAddressMap.end()) {
    return iter->second;
  }
  return Ipv4Address::GetAny();
}

std::string
DVRoutingProtocol::ReverseLookup(Ipv4Address ipAddress) {
  std::map<Ipv4Address, uint32_t>::iterator iter = m_addressNodeMap.find(ipAddress);
  if (iter != m_addressNodeMap.end()) {
    std::ostringstream sin;
    uint32_t nodeNumber = iter->second;
    sin << nodeNumber;
    return sin.str();
  }
  return "Unknown";
}

void DVRoutingProtocol::DoInitialize() {
  if (m_mainAddress == Ipv4Address()) {
    Ipv4Address loopback("127.0.0.1");
    for (uint32_t i = 0; i < m_ipv4->GetNInterfaces(); i++) {
      // Use primary address, if multiple
      Ipv4Address addr = m_ipv4->GetAddress(i, 0).GetLocal();
      if (addr != loopback) {
        m_mainAddress = addr;
        break;
      }
    }
    NS_ASSERT(m_mainAddress != Ipv4Address());
  }

  NS_LOG_DEBUG("Starting DV on node " << m_mainAddress);

  bool canRunDV = false;
  // Create sockets
  for (uint32_t i = 0; i < m_ipv4->GetNInterfaces(); i++) {
    Ipv4Address ipAddress = m_ipv4->GetAddress(i, 0).GetLocal();
    if (ipAddress == Ipv4Address::GetLoopback())
      continue;

    // Create a socket to listen on all the interfaces
    if (m_recvSocket == 0) {
      m_recvSocket = Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
      m_recvSocket->SetAllowBroadcast(true);
      InetSocketAddress inetAddr(Ipv4Address::GetAny(), DV_PORT_NUMBER);
      m_recvSocket->SetRecvCallback(MakeCallback(&DVRoutingProtocol::RecvDVMessage, this));
      if (m_recvSocket->Bind(inetAddr)) {
        NS_FATAL_ERROR("Failed to bind() LS socket");
      }
      m_recvSocket->SetRecvPktInfo(true);
      m_recvSocket->ShutdownSend();
    }

    // Create socket on this interface
    Ptr<Socket> socket = Socket::CreateSocket(GetObject<Node>(), UdpSocketFactory::GetTypeId());
    socket->SetAllowBroadcast(true);
    InetSocketAddress inetAddr(m_ipv4->GetAddress(i, 0).GetLocal(), m_dvPort);
    socket->SetRecvCallback(MakeCallback(&DVRoutingProtocol::RecvDVMessage, this));
    if (socket->Bind(inetAddr)) {
      NS_FATAL_ERROR("DVRoutingProtocol::DoInitialize::Failed to bind socket!");
    }
    socket->BindToNetDevice(m_ipv4->GetNetDevice(i));
    m_socketAddresses[socket] = m_ipv4->GetAddress(i, 0);
    canRunDV = true;
  }

  if (canRunDV) {
    AuditPings();
    NS_LOG_DEBUG("Starting DV on node " << m_mainAddress);
  }

  m_helloTimer.SetFunction(&DVRoutingProtocol::BroadcastHello, this);
  m_helloTimer.Schedule(m_helloInterval);

  m_auditTimer.SetFunction(&DVRoutingProtocol::AuditNeighbors, this);
  m_auditTimer.Schedule(m_auditInterval);

  // Triggered update timer
  m_triggerTimer.SetFunction(&DVRoutingProtocol::SendDVAdvertisements, this);
}

void DVRoutingProtocol::ScheduleTriggeredUpdate() {
  if (!m_triggerTimer.IsRunning()) {
    m_triggerTimer.Schedule(m_triggerInterval);
  }
}

// You can ignore this function
void DVRoutingProtocol::PrintRoutingTable(Ptr<OutputStreamWrapper> stream, Time::Unit unit) const {}

Ptr<Ipv4Route>
DVRoutingProtocol::RouteOutput(Ptr<Packet> packet, const Ipv4Header &header, Ptr<NetDevice> outInterface, Socket::SocketErrno &sockerr) {
  Ptr<Ipv4Route> ipv4Route = m_staticRouting->RouteOutput(packet, header, outInterface, sockerr);
  if (ipv4Route) {
    DEBUG_LOG("Found route to: " << ipv4Route->GetDestination() << " via next-hop: " << ipv4Route->GetGateway() << " with source: " << ipv4Route->GetSource() << " and output device " << ipv4Route->GetOutputDevice());
  } else {
    DEBUG_LOG("No Route to destination: " << header.GetDestination());
  }
  return ipv4Route;
}

bool DVRoutingProtocol::RouteInput(Ptr<const Packet> packet,
                                  const Ipv4Header &header, Ptr<const NetDevice> inputDev,
                                  UnicastForwardCallback ucb, MulticastForwardCallback mcb,
                                  LocalDeliverCallback lcb, ErrorCallback ecb) {
  Ipv4Address destinationAddress = header.GetDestination();
  Ipv4Address sourceAddress = header.GetSource();

  // Drop if packet was originated by this node
  if (IsOwnAddress(sourceAddress) == true) {
    return true;
  }

  // Check for local delivery
  uint32_t interfaceNum = m_ipv4->GetInterfaceForDevice(inputDev);
  if (m_ipv4->IsDestinationAddress(destinationAddress, interfaceNum)) {
    if (!lcb.IsNull()) {
      lcb(packet, header, interfaceNum);
      return true;
    } else {
      return false;
    }
  }

  // Check static routing table
  if (m_staticRouting->RouteInput(packet, header, inputDev, ucb, mcb, lcb, ecb)) {
    return true;
  }
  DEBUG_LOG("Cannot forward packet. No Route to destination: " << header.GetDestination());
  return false;
}

void DVRoutingProtocol::BroadcastPacket(Ptr<Packet> packet) {
  for (std::map<Ptr<Socket>, Ipv4InterfaceAddress>::const_iterator i =
      m_socketAddresses.begin();
      i != m_socketAddresses.end(); i++) {
    Ptr<Packet> pkt = packet->Copy();
    Ipv4Address broadcastAddr = i->second.GetLocal().GetSubnetDirectedBroadcast(i->second.GetMask());
    i->first->SendTo(pkt, 0, InetSocketAddress(broadcastAddr, DV_PORT_NUMBER));
  }
}

void DVRoutingProtocol::ProcessCommand(std::vector<std::string> tokens) {
  std::vector<std::string>::iterator iterator = tokens.begin();
  std::string command = *iterator;
  if (command == "PING") {
    if (tokens.size() < 3) {
      ERROR_LOG("Insufficient PING params...");
      return;
    }
    iterator++;
    std::istringstream sin(*iterator);
    uint32_t nodeNumber;
    sin >> nodeNumber;
    iterator++;
    std::string pingMessage = *iterator;
    Ipv4Address destAddress = ResolveNodeIpAddress(nodeNumber);
    if (destAddress != Ipv4Address::GetAny()) {
      uint32_t sequenceNumber = GetNextSequenceNumber();
      TRAFFIC_LOG("Sending PING_REQ to Node: " << nodeNumber << " IP: " << destAddress << " Message: " << pingMessage << " SequenceNumber: " << sequenceNumber);
      Ptr<PingRequest> pingRequest = Create<PingRequest>(sequenceNumber, Simulator::Now(), destAddress, pingMessage);
      // Add to ping-tracker
      m_pingTracker.insert(std::make_pair(sequenceNumber, pingRequest));
      Ptr<Packet> packet = Create<Packet>();
      DVMessage dvMessage = DVMessage(DVMessage::PING_REQ, sequenceNumber, m_maxTTL, m_mainAddress);
      dvMessage.SetPingReq(destAddress, pingMessage);
      packet->AddHeader(dvMessage);
      BroadcastPacket(packet);
    }
  } else if (command == "DUMP") {
    if (tokens.size() < 2) {
      ERROR_LOG("Insufficient Parameters!");
      return;
    }
    iterator++;
    std::string table = *iterator;
    if (table == "ROUTES" || table == "ROUTING") {
      DumpRoutingTable();
    } else if (table == "NEIGHBORS" || table == "NEIGHBOURS") {
      DumpNeighbors();
    }
  }
}

void DVRoutingProtocol::AuditNeighbors() {
  // Method to REMOVE unused neighbors only. Won't add new neighbors.
  Time now = Simulator::Now();
  bool dvUpdated = false;

  for (auto it = m_neighborTable.begin(); it != m_neighborTable.end();) {
    if (now - it->second.lastHeard > m_helloTimeout) {
      Ipv4Address neighborAddr = it->first;
      NS_LOG_DEBUG("Neighbor timeout: " << neighborAddr);
      // Remove routes via this neighbor
      for (auto dvIt = m_distanceVector.begin(); dvIt != m_distanceVector.end();) {
        if (dvIt->second.nextHop == neighborAddr) {
          dvIt = m_distanceVector.erase(dvIt);
          dvUpdated = true;
        } else {
          ++dvIt;
        }
      }
      it = m_neighborTable.erase(it);
    } else {
      ++it;
    }
  }
  if (dvUpdated) {
    UpdateRoutingTable();
    ScheduleTriggeredUpdate();
    NS_LOG_DEBUG("Neighbor was removed, routing table updated");
  }
  m_auditTimer.Schedule(m_auditInterval);
}

void DVRoutingProtocol::ProcessDVAdvert(DVMessage dvMessage) {
  Ipv4Address originator = dvMessage.GetOriginatorAddress();
  auto neighborIt = m_neighborTable.find(originator);
  if (neighborIt == m_neighborTable.end()) return; // Ignore if originator is not a neighbor

  neighborIt->second.lastHeard = Simulator::Now();
  auto receivedDV = dvMessage.GetDVAdvert().distanceVector; // update last time neighbor is heard from
  bool dvUpdated = false;
  uint32_t selfNode = m_addressNodeMap[m_mainAddress]; // get address of current node

  // Step 1: Iterate through the received distance vector
  for (const auto& entry : receivedDV) {
    uint32_t destNode = entry.first;
    if (destNode == selfNode) continue; // skip itself

    // new cost to reach destination
    uint16_t newCost = std::min(static_cast<uint16_t>(16), 
                                static_cast<uint16_t>(entry.second + 1));

    auto currentIt = m_distanceVector.find(destNode);

    if (currentIt == m_distanceVector.end()) {
      // distance vector entry does not exist: add if <= 16
      if (newCost < 16) {
        m_distanceVector[destNode] = {originator, newCost};
        dvUpdated = true;
      }
    } else {
      // distance vector entry exists
      RoutingEntry& current = currentIt->second;
      if (current.nextHop == originator) {
        // existing vector entry has the SAME ORIGINATOR
        if (newCost >= 16) {
          // new cost > 16, erase entry
          m_distanceVector.erase(currentIt);
          dvUpdated = true;
        } else if (current.cost != newCost) {
          // exists but cost is different, update cost
          current.cost = newCost;
          dvUpdated = true;
        }
      } else if (newCost < current.cost) {
        // originator is different and new cost < current cost
        current.nextHop = originator;
        current.cost = newCost;
        dvUpdated = true;
      }
    }
  }

  // Step 2: Handle the case where the originator didn't send a distance for any of the destinations it previously advertised
  for (auto it = m_distanceVector.begin(); it != m_distanceVector.end(); ) {
    uint32_t destNode = it->first;
    RoutingEntry& routingEntry = it->second;
    
    // If the current destination's next hop is the originator and it wasn't given in the received advertisement, delete it
    // Don't delete direct neighbors though
    if (routingEntry.nextHop == originator && receivedDV.find(destNode) == receivedDV.end() && routingEntry.cost > 1) {
      // This means the originator didn't advertise the distance for this destination, implying disconnection
      it = m_distanceVector.erase(it);  // Erase the entry
      dvUpdated = true;  // Mark that the DV was updated
    } else {
      ++it;
    }
  }

  if (dvUpdated) {
    UpdateRoutingTable();
    ScheduleTriggeredUpdate();
  }
}

void DVRoutingProtocol::SendDVAdvertisements() {
  std::map<uint32_t, uint16_t> poisonedDV;

  for (const auto& entry : m_distanceVector) {
    uint32_t dest = entry.first;
    // if next hop is the current neighbor, set cost to 16 (invalid) avoid loops
    uint16_t cost = entry.second.cost;
    poisonedDV[dest] = cost;
  }
  
  // Create DV message
  DVMessage dvMsg(DVMessage::DV_ADVERT, GetNextSequenceNumber(), m_maxTTL, m_mainAddress);
  dvMsg.SetDVAdvert(poisonedDV);
  Ptr<Packet> packet = Create<Packet>();
  packet->AddHeader(dvMsg);

  // Broadcast the packet to the neighbors
  BroadcastPacket(packet);
}

// Send Hello to every neighbor
// Also sends DV advertisements (rather than triggered updates)
void DVRoutingProtocol::BroadcastHello() {
  // Create a universal Hello message
  DVMessage dvMsg(DVMessage::HELLO, GetNextSequenceNumber(), m_maxTTL, m_mainAddress);
  dvMsg.SetHello("hello");

  // Create a packet and serialize the Hello message into it
  Ptr<Packet> packet = Create<Packet>();
  packet->AddHeader(dvMsg);

  // Broadcast the packet to the neighbors
  BroadcastPacket(packet);

  // Reschedule the timer
  m_helloTimer.Schedule(m_helloInterval);

  SendDVAdvertisements();
}

Ptr<Socket> DVRoutingProtocol::FindSocketForInterface(Ipv4Address interfaceAddr) {
  for (const auto& socketEntry : m_socketAddresses) {
    if (socketEntry.second.GetLocal() == interfaceAddr) {
      return socketEntry.first;
    }
  }
  return nullptr;
}

void DVRoutingProtocol::UpdateRoutingTable() {
  // Clear all existing routes from m_staticRouting
  // Get the number of routes
  while (m_staticRouting->GetNRoutes() > 0) {
    m_staticRouting->RemoveRoute(0);
  }

  // Add all routes to other nodes
  for (const auto& entry : m_distanceVector) {
    // don't add if cost >= 16
    if (entry.second.cost >= 16) continue;
    
    Ipv4Address destAddr = ResolveNodeIpAddress(entry.first); // get ip address
    auto neighborIt = m_neighborTable.find(entry.second.nextHop); // next hop
    if (neighborIt == m_neighborTable.end()) continue; // if next nop not a neighbor..

    // Use the interface address from neighbor table
    uint32_t interface = m_ipv4->GetInterfaceForAddress(neighborIt->second.interfaceAddr);
    m_staticRouting->AddHostRouteTo(destAddr,
                                    entry.second.nextHop, 
                                    interface,
                                    entry.second.cost);
  }
}

void DVRoutingProtocol::DumpNeighbors() {
  PRINT_LOG(std::endl
            << "**************** Neighbor List ********************" << std::endl
            << "NeighborNumber\t\tNeighborAddr\t\tInterfaceAddr");
  
  // Print number of neighbors first
  PRINT_LOG(m_neighborTable.size());

  // Print each neighbor entry
  for (std::map<Ipv4Address, NeighborEntry>::const_iterator it = m_neighborTable.begin();
      it != m_neighborTable.end(); it++) {
    const NeighborEntry& entry = it->second;
    PRINT_LOG(entry.neighborNumber << "\t\t" 
              << entry.neighborAddr << "\t\t"
              << entry.interfaceAddr);
    checkNeighborTableEntry(entry.neighborNumber, entry.neighborAddr, entry.interfaceAddr);
  }

  PRINT_LOG("");
}

void DVRoutingProtocol::DumpRoutingTable() {
  // Print the header for the routing table
  PRINT_LOG(std::endl
            << "**************** Route Table ********************" << std::endl
            << "DestNumber\t\tDestAddr\t\tNextHopNumber\t\tNextHopAddr\t\tInterfaceAddr\t\tCost");

  // Print the number of entries in the distance vector
  PRINT_LOG(m_distanceVector.size());

  // Iterate over the distance vector to print each route entry
  for (const auto& entry : m_distanceVector) {
    uint32_t destNode = entry.first; // Destination node
    const RoutingEntry& routingEntry = entry.second;
    Ipv4Address destAddr = ResolveNodeIpAddress(destNode); // Resolve destination IP address
    
    Ipv4Address nextHopAddr = routingEntry.nextHop;
    uint32_t nextHopNode = m_addressNodeMap[nextHopAddr];

    // Find the interface address from the neighbor table based on the next hop
    Ipv4Address interfaceAddr;
    auto neighborIt = m_neighborTable.find(nextHopAddr);
    if (neighborIt != m_neighborTable.end()) {
      interfaceAddr = neighborIt->second.interfaceAddr;
    }
    
    uint16_t cost = routingEntry.cost;

    // Print the routing table entry in the required format
    PRINT_LOG(destNode << "\t\t" 
              << destAddr << "\t\t" 
              << nextHopNode << "\t\t" 
              << nextHopAddr << "\t\t" 
              << interfaceAddr << "\t\t" 
              << cost);
    checkRouteTableEntry(destNode, destAddr, nextHopNode, nextHopAddr, interfaceAddr, cost);
  }

  // Print a blank line after the table
  PRINT_LOG("");
}

void DVRoutingProtocol::RecvDVMessage(Ptr<Socket> socket) {
  Address sourceAddr;
  Ptr<Packet> packet = socket->RecvFrom(sourceAddr);
  DVMessage dvMessage;
  Ipv4PacketInfoTag interfaceInfo;
  if (!packet->RemovePacketTag(interfaceInfo)) {
    NS_ABORT_MSG("No incoming interface on OLSR message, aborting.");
  }
  uint32_t incomingIf = interfaceInfo.GetRecvIf();

  if (!packet->RemoveHeader(dvMessage)) {
    NS_ABORT_MSG("No incoming interface on LS message, aborting.");
  }

  Ipv4Address interface;
  uint32_t idx = 1;
  for (std::map<Ptr<Socket>, Ipv4InterfaceAddress>::iterator iter = m_socketAddresses.begin();
      iter != m_socketAddresses.end(); iter++) {
    if (idx == incomingIf) {
      interface = iter->second.GetLocal(); // find the incoming interface
      break;
    }
    idx++;
  }

  switch (dvMessage.GetMessageType()) {
    case DVMessage::PING_REQ:
      ProcessPingReq(dvMessage);
      break;
    case DVMessage::PING_RSP:
      ProcessPingRsp(dvMessage);
      break;
    case DVMessage::DV_ADVERT:
      ProcessDVAdvert(dvMessage);
      break;
    case DVMessage::HELLO:
      ProcessHello(dvMessage, interface);
      break;
    default:
      ERROR_LOG("Unknown Message Type!");
      break;
  }
}

void DVRoutingProtocol::ProcessHello(DVMessage dvMessage, Ipv4Address interfaceAddr) {
  // Use reverse lookup for ease of debug
  std::string fromNode = ReverseLookup(dvMessage.GetOriginatorAddress());
  // TRAFFIC_LOG("Received HELLO, From Node: " << fromNode << ", Message: " << dvMessage.GetHello());

  // Update lastHeard to be the current time in m_neighborTable
  auto neighborIt = m_neighborTable.find(dvMessage.GetOriginatorAddress());
  if (neighborIt != m_neighborTable.end()) {
    // neighbor is already known
    neighborIt->second.lastHeard = Simulator::Now();
  } else {
    // Add the NEW neighbor to the neighbor table
    NeighborEntry entry;
    entry.neighborNumber = m_addressNodeMap[dvMessage.GetOriginatorAddress()];
    entry.neighborAddr = dvMessage.GetOriginatorAddress();
    entry.interfaceAddr = interfaceAddr;
    entry.lastHeard = Simulator::Now();
    m_neighborTable[dvMessage.GetOriginatorAddress()] = entry;

    // Update the m_distanceVector
    m_distanceVector[entry.neighborNumber] = {entry.neighborAddr, 1};
    
    UpdateRoutingTable();
    ScheduleTriggeredUpdate();
    NS_LOG_DEBUG("Neighbor was added, routing table updated");
  }
}

void DVRoutingProtocol::ProcessPingReq(DVMessage dvMessage) {
  // Check destination address
  if (IsOwnAddress(dvMessage.GetPingReq().destinationAddress)) {
    // Use reverse lookup for ease of debug
    std::string fromNode = ReverseLookup(dvMessage.GetOriginatorAddress());
    TRAFFIC_LOG("Received PING_REQ, From Node: " << fromNode << ", Message: " << dvMessage.GetPingReq().pingMessage);
    // Send Ping Response
    DVMessage dvResp = DVMessage(DVMessage::PING_RSP, dvMessage.GetSequenceNumber(), m_maxTTL, m_mainAddress);
    dvResp.SetPingRsp(dvMessage.GetOriginatorAddress(), dvMessage.GetPingReq().pingMessage);
    Ptr<Packet> packet = Create<Packet>();
    packet->AddHeader(dvResp);
    BroadcastPacket(packet);
  }
}

void DVRoutingProtocol::ProcessPingRsp(DVMessage dvMessage) {
  // Check destination address
  if (IsOwnAddress(dvMessage.GetPingRsp().destinationAddress)) {
    // Remove from pingTracker
    std::map<uint32_t, Ptr<PingRequest>>::iterator iter;
    iter = m_pingTracker.find(dvMessage.GetSequenceNumber());
    if (iter != m_pingTracker.end()) {
      std::string fromNode = ReverseLookup(dvMessage.GetOriginatorAddress());
      TRAFFIC_LOG("Received PING_RSP, From Node: " << fromNode << ", Message: " << dvMessage.GetPingRsp().pingMessage);
      m_pingTracker.erase(iter);
    } else {
      DEBUG_LOG("Received invalid PING_RSP!");
    }
  }
}

bool DVRoutingProtocol::IsOwnAddress(Ipv4Address originatorAddress) {
  // Check all interfaces
  for (std::map<Ptr<Socket>, Ipv4InterfaceAddress>::const_iterator i = m_socketAddresses.begin(); i != m_socketAddresses.end(); i++) {
    Ipv4InterfaceAddress interfaceAddr = i->second;
    if (originatorAddress == interfaceAddr.GetLocal()) {
      return true;
    }
  }
  return false;
}

void DVRoutingProtocol::AuditPings() {
  std::map<uint32_t, Ptr<PingRequest>>::iterator iter;
  for (iter = m_pingTracker.begin(); iter != m_pingTracker.end();) {
    Ptr<PingRequest> pingRequest = iter->second;
    if (pingRequest->GetTimestamp().GetMilliSeconds() + m_pingTimeout.GetMilliSeconds() <= Simulator::Now().GetMilliSeconds()) {
      DEBUG_LOG("Ping expired. Message: " << pingRequest->GetPingMessage() << " Timestamp: " << pingRequest->GetTimestamp().GetMilliSeconds() << " CurrentTime: " << Simulator::Now().GetMilliSeconds());
      // Remove stale entries
      m_pingTracker.erase(iter++);
    } else {
      ++iter;
    }
  }
  // Rechedule timer
  m_auditPingsTimer.Schedule(m_pingTimeout);
}

uint32_t
DVRoutingProtocol::GetNextSequenceNumber() {
  m_currentSequenceNumber = (m_currentSequenceNumber + 1) % (DV_MAX_SEQUENCE_NUMBER + 1);
  return m_currentSequenceNumber;
}

void DVRoutingProtocol::NotifyInterfaceUp(uint32_t i) {
  m_staticRouting->NotifyInterfaceUp(i);
}

void DVRoutingProtocol::NotifyInterfaceDown(uint32_t i) {
  m_staticRouting->NotifyInterfaceDown(i);
}

void DVRoutingProtocol::NotifyAddAddress(uint32_t interface, Ipv4InterfaceAddress address) {
  m_staticRouting->NotifyAddAddress(interface, address);
}

void DVRoutingProtocol::NotifyRemoveAddress(uint32_t interface, Ipv4InterfaceAddress address) {
  m_staticRouting->NotifyRemoveAddress(interface, address);
}

void DVRoutingProtocol::SetIpv4(Ptr<Ipv4> ipv4) {
  NS_ASSERT(ipv4 != 0);
  NS_ASSERT(m_ipv4 == 0);
  NS_LOG_DEBUG("Created dv::RoutingProtocol");
  // Configure timers
  m_auditPingsTimer.SetFunction(&DVRoutingProtocol::AuditPings, this);
  m_ipv4 = ipv4;
  m_staticRouting->SetIpv4(m_ipv4);
}
