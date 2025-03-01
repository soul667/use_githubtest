#include "ns3/dv-message.h"
#include "ns3/log.h"
#include <map>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("DVMessage");
NS_OBJECT_ENSURE_REGISTERED (DVMessage);

DVMessage::DVMessage () {}

DVMessage::~DVMessage () {}

DVMessage::DVMessage (DVMessage::MessageType messageType, uint32_t sequenceNumber, uint8_t ttl, Ipv4Address originatorAddress)
{
  m_messageType = messageType;
  m_sequenceNumber = sequenceNumber;
  m_ttl = ttl;
  m_originatorAddress = originatorAddress;
}

TypeId 
DVMessage::GetTypeId (void)
{
  static TypeId tid = TypeId ("DVMessage")
    .SetParent<Header> ()
    .AddConstructor<DVMessage> ()
  ;
  return tid;
}

TypeId
DVMessage::GetInstanceTypeId (void) const
{
  return GetTypeId ();
}


uint32_t
DVMessage::GetSerializedSize (void) const
{
  // size of messageType, sequence number, originator address, ttl
  uint32_t size = sizeof (uint8_t) + sizeof (uint32_t) + IPV4_ADDRESS_SIZE + sizeof (uint8_t);
  switch (m_messageType) {
    case PING_REQ:
      size += m_message.pingReq.GetSerializedSize ();
      break;
    case PING_RSP:
      size += m_message.pingRsp.GetSerializedSize ();
      break;
    case DV_ADVERT:
      size += m_message.dvAdvert.GetSerializedSize ();
      break;
    case HELLO:
      size += m_message.hello.GetSerializedSize ();
      break;
    default:
      NS_ASSERT (false);
  }
  return size;
}

void
DVMessage::Print (std::ostream &os) const
{
  os << "\n****DVMessage Dump****\n" ;
  os << "messageType: " << m_messageType << "\n";
  os << "sequenceNumber: " << m_sequenceNumber << "\n";
  os << "ttl: " << m_ttl << "\n";
  os << "originatorAddress: " << m_originatorAddress << "\n";
  os << "PAYLOAD:: \n";
  
  switch (m_messageType) {
    case PING_REQ:
      m_message.pingReq.Print (os);
      break;
    case PING_RSP:
      m_message.pingRsp.Print (os);
      break;
    case DV_ADVERT:
      m_message.dvAdvert.Print (os);
      break;
    case HELLO:
      m_message.hello.Print (os);
      break;
    default:
      break;  
  }
  os << "\n****END OF MESSAGE****\n";
}

void
DVMessage::Serialize (Buffer::Iterator start) const
{
  Buffer::Iterator i = start;
  i.WriteU8 (m_messageType);
  i.WriteHtonU32 (m_sequenceNumber);
  i.WriteU8 (m_ttl);
  i.WriteHtonU32 (m_originatorAddress.Get ());

  switch (m_messageType) {
    case PING_REQ:
      m_message.pingReq.Serialize (i);
      break;
    case PING_RSP:
      m_message.pingRsp.Serialize (i);
      break;
    case DV_ADVERT:
      m_message.dvAdvert.Serialize (i);
      break;
    case HELLO:
      m_message.hello.Serialize (i);
      break;
    default:
      NS_ASSERT (false);   
  }
}

uint32_t 
DVMessage::Deserialize (Buffer::Iterator start)
{
  uint32_t size;
  Buffer::Iterator i = start;
  m_messageType = (MessageType) i.ReadU8 ();
  m_sequenceNumber = i.ReadNtohU32 ();
  m_ttl = i.ReadU8 ();
  m_originatorAddress = Ipv4Address (i.ReadNtohU32 ());

  size = sizeof (uint8_t) + sizeof (uint32_t) + sizeof (uint8_t) + IPV4_ADDRESS_SIZE;

  switch (m_messageType) {
    case PING_REQ:
      size += m_message.pingReq.Deserialize (i);
      break;
    case PING_RSP:
      size += m_message.pingRsp.Deserialize (i);
      break;
    case DV_ADVERT:
      size += m_message.dvAdvert.Deserialize (i);
      break;
    case HELLO:
      size += m_message.hello.Deserialize (i);
      break;
    default:
      NS_ASSERT (false);
  }
  return size;
}

/* DV_ADVERT */

uint32_t
DVMessage::DVAdvert::GetSerializedSize() const
{
  // 4 bytes for number of entries + 6 bytes per entry (4 for node number, 2 for cost)
  return 4 + (distanceVector.size() * (sizeof(uint32_t) + sizeof(uint16_t)));
}

void
DVMessage::DVAdvert::Print(std::ostream &os) const
{
  os << "Distance Vector Advertisement::\n";
  for (const auto& entry : distanceVector) {
    os << "Node: " << entry.first << " Cost: " << entry.second << "\n";
  }
}

void
DVMessage::DVAdvert::Serialize(Buffer::Iterator &start) const
{
  start.WriteHtonU32(distanceVector.size());  // Write number of entries

  for (const auto& entry : distanceVector) {
    start.WriteHtonU32(entry.first);  // Node number
    start.WriteHtonU16(entry.second); // Cost
  }
}

uint32_t
DVMessage::DVAdvert::Deserialize(Buffer::Iterator &start)
{
  distanceVector.clear();
  uint32_t size = 0;

  uint32_t numEntries = start.ReadNtohU32();  // Read number of entries
  size += sizeof(numEntries);

  for (uint32_t i = 0; i < numEntries; i++) {
    if (start.GetSize() < sizeof(uint32_t) + sizeof(uint16_t)) {
        NS_LOG_ERROR("Not enough bytes to deserialize entry");
        break; // Prevent out-of-bounds read
    }
    
    uint32_t nodeNumber = start.ReadNtohU32(); // Read node number
    uint16_t cost = start.ReadNtohU16();       // Read cost
    distanceVector[nodeNumber] = cost;
    
    size += sizeof(nodeNumber) + sizeof(cost);
  }

  return size;
}

// void
// DVMessage::DVAdvert::Serialize(Buffer::Iterator &start) const
// {
//   for (const auto& entry : distanceVector) {
//     start.WriteHtonU32(entry.first);
//     start.WriteHtonU16(entry.second);
//   }
// }

// uint32_t
// DVMessage::DVAdvert::Deserialize(Buffer::Iterator &start)
// {
//   distanceVector.clear();
//   uint32_t size = 0;

//   // Iterate while the current iterator is not at the end
//   while (!start.IsEnd()) {
//     uint32_t nodeNumber = start.ReadNtohU32();  // Read node number
//     uint16_t cost = start.ReadNtohU16();        // Read cost
//     distanceVector[nodeNumber] = cost;          // Store in map
//     size += sizeof(nodeNumber) + sizeof(cost);  // Update size
//     start.Next();  // Move to the next iterator position
//   }

//   return size;
// }

void
DVMessage::SetDVAdvert(std::map<uint32_t, uint16_t> distanceVector)
{
  if (m_messageType == 0)
  {
    m_messageType = DV_ADVERT;
  }
  else
  {
    NS_ASSERT(m_messageType == DV_ADVERT);
  }
  m_message.dvAdvert.distanceVector = distanceVector;
}

DVMessage::DVAdvert
DVMessage::GetDVAdvert()
{
  return m_message.dvAdvert;
}

/* HELLO */
uint32_t DVMessage::Hello::GetSerializedSize() const {
  return sizeof(uint16_t) + helloMessage.length();
}

void DVMessage::Hello::Print(std::ostream &os) const {
  os << "Hello Message: " << helloMessage << "\n";
}

void DVMessage::Hello::Serialize(Buffer::Iterator &start) const {
  start.WriteU16(helloMessage.length());
  start.Write((uint8_t *)(const_cast<char*>(helloMessage.c_str())), helloMessage.length());
}

uint32_t DVMessage::Hello::Deserialize(Buffer::Iterator &start) {
  uint16_t length = start.ReadU16();
  char* str = (char*) malloc(length);
  start.Read((uint8_t*)str, length);
  helloMessage = std::string(str, length);
  free(str);
  return GetSerializedSize();
}

void
DVMessage::SetHello(std::string helloMessage) {
  if (m_messageType == 0) {
    m_messageType = HELLO;
  } else {
    NS_ASSERT(m_messageType == HELLO);
  }
  m_message.hello.helloMessage = helloMessage;
}

DVMessage::Hello
DVMessage::GetHello() {
  return m_message.hello;
}


/* PING_REQ */

uint32_t 
DVMessage::PingReq::GetSerializedSize (void) const
{
  uint32_t size;
  size = IPV4_ADDRESS_SIZE + sizeof(uint16_t) + pingMessage.length();
  return size;
}

void
DVMessage::PingReq::Print (std::ostream &os) const
{
  os << "PingReq:: Message: " << pingMessage << "\n";
}

void
DVMessage::PingReq::Serialize (Buffer::Iterator &start) const
{
  start.WriteHtonU32 (destinationAddress.Get ());
  start.WriteU16 (pingMessage.length ());
  start.Write ((uint8_t *) (const_cast<char*> (pingMessage.c_str())), pingMessage.length());
}

uint32_t
DVMessage::PingReq::Deserialize (Buffer::Iterator &start)
{  
  destinationAddress = Ipv4Address (start.ReadNtohU32 ());
  uint16_t length = start.ReadU16 ();
  char* str = (char*) malloc (length);
  start.Read ((uint8_t*)str, length);
  pingMessage = std::string (str, length);
  free (str);
  return PingReq::GetSerializedSize ();
}

void
DVMessage::SetPingReq (Ipv4Address destinationAddress, std::string pingMessage)
{
  if (m_messageType == 0)
    {
      m_messageType = PING_REQ;
    }
  else
    {
      NS_ASSERT (m_messageType == PING_REQ);
    }
  m_message.pingReq.destinationAddress = destinationAddress;
  m_message.pingReq.pingMessage = pingMessage;
}

DVMessage::PingReq
DVMessage::GetPingReq ()
{
  return m_message.pingReq;
}

/* PING_RSP */

uint32_t 
DVMessage::PingRsp::GetSerializedSize (void) const
{
  uint32_t size;
  size = IPV4_ADDRESS_SIZE + sizeof(uint16_t) + pingMessage.length();
  return size;
}

void
DVMessage::PingRsp::Print (std::ostream &os) const
{
  os << "PingReq:: Message: " << pingMessage << "\n";
}

void
DVMessage::PingRsp::Serialize (Buffer::Iterator &start) const
{
  start.WriteHtonU32 (destinationAddress.Get ());
  start.WriteU16 (pingMessage.length ());
  start.Write ((uint8_t *) (const_cast<char*> (pingMessage.c_str())), pingMessage.length());
}

uint32_t
DVMessage::PingRsp::Deserialize (Buffer::Iterator &start)
{  
  destinationAddress = Ipv4Address (start.ReadNtohU32 ());
  uint16_t length = start.ReadU16 ();
  char* str = (char*) malloc (length);
  start.Read ((uint8_t*)str, length);
  pingMessage = std::string (str, length);
  free (str);
  return PingRsp::GetSerializedSize ();
}

void
DVMessage::SetPingRsp (Ipv4Address destinationAddress, std::string pingMessage)
{
  if (m_messageType == 0)
    {
      m_messageType = PING_RSP;
    }
  else
    {
      NS_ASSERT (m_messageType == PING_RSP);
    }
  m_message.pingRsp.destinationAddress = destinationAddress;
  m_message.pingRsp.pingMessage = pingMessage;
}

DVMessage::PingRsp
DVMessage::GetPingRsp ()
{
  return m_message.pingRsp;
}

//
//
//


void
DVMessage::SetMessageType (MessageType messageType)
{
  m_messageType = messageType;
}

DVMessage::MessageType
DVMessage::GetMessageType () const
{
  return m_messageType;
}

void
DVMessage::SetSequenceNumber (uint32_t sequenceNumber)
{
  m_sequenceNumber = sequenceNumber;
}

uint32_t 
DVMessage::GetSequenceNumber (void) const
{
  return m_sequenceNumber;
}

void
DVMessage::SetTTL (uint8_t ttl)
{
  m_ttl = ttl;
}

uint8_t 
DVMessage::GetTTL (void) const
{
  return m_ttl;
}

void
DVMessage::SetOriginatorAddress (Ipv4Address originatorAddress)
{
  m_originatorAddress = originatorAddress;
}

Ipv4Address
DVMessage::GetOriginatorAddress (void) const
{
  return m_originatorAddress;
}

