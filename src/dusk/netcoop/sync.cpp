#ifdef DUSK_NETCOOP

#include "internal.hpp"
#include "protocol.hpp"

#include <cstring>

namespace dusk::netcoop::internal {

void EncodeOutboundSnapshot(std::vector<uint8_t>* frame, const LinkState& s) {
    proto::Header h{};
    h.type    = uint8_t(proto::MsgType::LinkState);
    h.bodyLen = sizeof(proto::LinkStateMsg);

    proto::LinkStateMsg body;
    std::memcpy(&body, &s, sizeof(body));

    frame->resize(sizeof(h) + sizeof(body));
    std::memcpy(frame->data(),                 &h,    sizeof(h));
    std::memcpy(frame->data() + sizeof(h),     &body, sizeof(body));
}

bool DecodeInboundFrame(const uint8_t* data, size_t len, LinkState* out) {
    if (len < sizeof(proto::Header)) return false;
    proto::Header h;
    std::memcpy(&h, data, sizeof(h));
    if (h.bodyLen != len - sizeof(h)) return false;
    if (h.type != uint8_t(proto::MsgType::LinkState)) return false;
    if (h.bodyLen != sizeof(proto::LinkStateMsg)) return false;

    proto::LinkStateMsg body;
    std::memcpy(&body, data + sizeof(h), sizeof(body));
    std::memcpy(out, &body, sizeof(*out));
    return true;
}

}  // namespace dusk::netcoop::internal

#endif  // DUSK_NETCOOP
