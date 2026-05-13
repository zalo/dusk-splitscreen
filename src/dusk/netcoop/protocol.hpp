// Wire protocol between two Dusk instances. Both endpoints are trusted, so the
// wire format is a fixed-layout C struct rather than a self-describing format
// like protobuf. Endianness is little-endian; both ends are x86_64 in practice.

#pragma once

#ifdef DUSK_NETCOOP

#include <cstdint>
#include "dusk/netcoop.hpp"

namespace dusk::netcoop::proto {

// Bumped on any wire-incompatible change. Handshake aborts on mismatch.
constexpr uint32_t kVersion = 1;

enum class MsgType : uint8_t {
    Hello       = 0x01,  // first message after WS handshake (each direction)
    LinkState   = 0x02,  // periodic local-player snapshot
    Bye         = 0x03,  // clean shutdown notice
    // Reserved for later phases:
    // SaveSync  = 0x10, WorldEvent = 0x11, NpcState = 0x12, Chat = 0x20
};

#pragma pack(push, 1)

struct Header {
    uint8_t  type;       // MsgType
    uint8_t  pad[3];     // align to 4
    uint32_t bodyLen;    // bytes following the header
};

struct Hello {
    uint32_t version;        // kVersion
    uint64_t instanceUuid;   // random per-process, used for tiebreaker
    uint16_t selfPort;       // the port this instance bound for inbound
    uint8_t  saveSlot;       // which save the user is playing (0..2)
    uint8_t  pad;
    char     buildId[32];    // git short hash + dirty marker
};

// Mirrors dusk::netcoop::LinkState (same layout). Kept in protocol.hpp so the
// wire format is documented in one place.
struct LinkStateMsg {
    uint64_t serverFrame;
    float    pos[3];
    float    yaw;
    uint16_t animIdx;
    uint16_t flags;
    float    animFrame;
};

#pragma pack(pop)

static_assert(sizeof(Header) == 8, "wire Header layout drift");
static_assert(sizeof(Hello) == 48, "wire Hello layout drift");
static_assert(sizeof(LinkStateMsg) == sizeof(dusk::netcoop::LinkState),
              "LinkStateMsg must mirror LinkState");

}  // namespace dusk::netcoop::proto

#endif  // DUSK_NETCOOP
