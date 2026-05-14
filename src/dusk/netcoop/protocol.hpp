// Wire protocol between two Dusk instances. Both endpoints are trusted, so the
// wire format is a fixed-layout C struct rather than a self-describing format
// like protobuf. Endianness is little-endian; both ends are x86_64 in practice.

#pragma once

#ifdef DUSK_NETCOOP

#include <cstdint>
#include "dusk/netcoop.hpp"

namespace dusk::netcoop::proto {

// Bumped on any wire-incompatible change. Handshake aborts on mismatch.
constexpr uint32_t kVersion = 2;

enum class MsgType : uint8_t {
    Hello        = 0x01,  // first message after WS handshake (each direction)
    LinkState    = 0x02,  // periodic local-player snapshot
    Bye          = 0x03,  // clean shutdown notice

    // Phase 5 — save state replication.
    SaveBit      = 0x10,  // set/clear a single event flag
    SaveCounter  = 0x11,  // overwrite a named counter (rupees, max-life, …)
    SaveItem     = 0x12,  // set an item slot (slot, item id, count)
    SaveEquip    = 0x13,  // set a select-equip slot (clothes / sword / shield / B / smell)
    SaveSnapshot = 0x14,  // full save state, sent at handshake for catch-up

    // Admin / debug.
    WarpRequest  = 0x20,  // peer asks us to teleport our local Link to (x,y,z,yaw)

    // Initial-load handoff: host's stage/room/point/layer + Link pos for P2
    // to inherit on first connect.
    WorldLocation = 0x21,
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

// SaveBit payload.
struct SaveBitMsg {
    uint16_t flag;   // raw u16 encoding (byte<<8 | mask) used by dSv_event_c
    uint8_t  on;     // 1=set, 0=clear
    uint8_t  pad;
};

// SaveCounter payload. `which` is a SaveCounterId.
struct SaveCounterMsg {
    uint16_t which;
    uint16_t pad;
    uint32_t value;
};

// SaveItem payload. Slot + item-id + count.
struct SaveItemMsg {
    uint8_t  slot;
    uint8_t  item;
    uint16_t count;
};

// SaveEquip payload. Which slot, what item.
struct SaveEquipMsg {
    uint8_t  which;
    uint8_t  item;
    uint16_t pad;
};

// Full event-bit + counter snapshot for handshake catch-up. Sent in BOTH
// directions immediately after the Hello exchange; receiver applies whichever
// side has more set bits / higher counters (idempotent merge).
struct SaveSnapshotMsg {
    uint8_t  eventBits[256];   // mirrors dSv_event_c::mEvent
    uint32_t counters[16];     // indexed by SaveCounterId; unused slots = 0
    uint8_t  equip[8];         // indexed by SaveEquipId
};

// Admin warp: peer asks us to teleport our local Link to a world point.
// Triggered from the Settings menu's "Warp Peer to Me" button.
struct WarpRequestMsg {
    float pos[3];   // target world position
    float yaw;      // target facing, radians
};

// Initial-load handoff: host broadcasts its current location after handshake;
// client uses it to load into the same stage/room as the host.
struct WorldLocationMsg {
    char  stage[8];   // null-padded stage name, e.g. "F_SP103"
    int16_t point;    // spawn-point ID
    int8_t  roomNo;
    int8_t  layer;
    float pos[3];
    float yaw;
};

#pragma pack(pop)

static_assert(sizeof(Header) == 8, "wire Header layout drift");
static_assert(sizeof(Hello) == 48, "wire Hello layout drift");
static_assert(sizeof(LinkStateMsg) == sizeof(dusk::netcoop::LinkState),
              "LinkStateMsg must mirror LinkState");
static_assert(sizeof(SaveBitMsg)     == 4,   "wire SaveBitMsg drift");
static_assert(sizeof(SaveCounterMsg) == 8,   "wire SaveCounterMsg drift");
static_assert(sizeof(SaveItemMsg)    == 4,   "wire SaveItemMsg drift");
static_assert(sizeof(SaveEquipMsg)   == 4,   "wire SaveEquipMsg drift");
static_assert(sizeof(SaveSnapshotMsg) == 256 + 64 + 8, "wire SaveSnapshotMsg drift");
static_assert(sizeof(WarpRequestMsg)  == 16,  "wire WarpRequestMsg drift");
static_assert(sizeof(WorldLocationMsg) == 28, "wire WorldLocationMsg drift");

}  // namespace dusk::netcoop::proto

#endif  // DUSK_NETCOOP
