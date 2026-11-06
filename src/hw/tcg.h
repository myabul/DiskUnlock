#pragma once
#include "../pch.h"
#include "../core/device.h"
#include "scsi.h"

namespace hw {
namespace tcg {

// Security protocol numbers used on the wire. 0x00 = TCG (Core spec), 0x01 = T12/Opal family.
// sedutil issues Level 0 Discovery with protocol 0x01 and protocol-specific 0x0001.
enum SecurityProtocol : uint8_t { protoTcg = 0x00, protoOpal = 0x01 };

enum FeatureCode : uint16_t {
    fcTPer = 0x0001,
    fcLocking = 0x0002,
    fcGeometry = 0x0003,
    fcEnterprise = 0x0100,
    fcOpalV100 = 0x0200,
    fcSingleUser = 0x0201,
    fcDataStore = 0x0202,
    fcOpalV200 = 0x0203,
};

struct DiscoveryInfo {
    bool responded = false;
    bool hasTPer = false;
    bool hasLocking = false;
    bool hasOpal = false;      // either Opal 1.0 or 2.0 descriptor present
    bool hasEnterprise = false;
    bool lockingSupported = false;
    bool lockingEnabled = false;
    bool locked = false;
    bool mediaEncryption = false;
    bool mbrEnabled = false;
    bool mbrDone = false;
    uint16_t baseComId = 0;
    uint16_t comIdCount = 0;
    uint32_t unknownDescriptors = 0;
    uint32_t vendorDescriptors = 0;
    std::wstring note;

    std::wstring summary() const;
};

// IF-RECV / IF-SEND abstracted over the three transports. `specific` is the COMID for packet
// traffic and 0x0001 for Level 0 Discovery.
CmdResult securityProtocolIn(core::Device& dev, uint8_t protocol, uint32_t specific,
                             void* buffer, uint32_t bufferLen, uint32_t timeoutSec = 30);
CmdResult securityProtocolOut(core::Device& dev, uint8_t protocol, uint32_t specific,
                              const void* buffer, uint32_t bufferLen, uint32_t timeoutSec = 30);

bool discoverLevel0(core::Device& dev, DiscoveryInfo& out);

struct RevertResult {
    bool attempted = false;
    bool sessionStarted = false;
    bool authenticated = false;
    bool revertAccepted = false;
    uint16_t statusCode = 0xFFFF;
    std::wstring detail;
};

// PSID Revert: StartSession(AdminSP) -> Authenticate(PSID authority, password as entered,
// no hashing) -> Revert(AdminSP) -> end. Irreversible: wipes all locks and PBR.
RevertResult psidRevert(core::Device& dev, const std::wstring& psid);

}  // namespace tcg
}  // namespace hw
