#include "tcg.h"
#include "ata.h"
#include "nvme.h"
#include "../core/str.h"

namespace hw {
namespace tcg {

namespace {

constexpr uint32_t kDiscoveryBuffer = 1024;
constexpr uint32_t kPacketHeaderBytes = 20 + 24 + 12;  // ComPacket + Packet + DataSubPacket
constexpr uint16_t kHostSessionId = 105;               // sedutil's fixed HSN

// TCG Core token bytes, as written on the wire by field-proven implementations.
enum Token : uint8_t {
    tokCall = 0xF8,
    tokEndOfData = 0xF9,
    tokEndOfSession = 0xFA,
    tokStartList = 0xF0,
    tokEndList = 0xF1,
    tokStartName = 0xF2,
    tokEndName = 0xF3,
    tokBytestring8 = 0xC8,
};

// Big-endian is mandatory: every multi-byte field in a ComPacket/Packet and in a discovery
// response is network order.
void putBe32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
void putBe16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
uint32_t getBe32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint16_t getBe16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }

// 8-byte UID constants verified against the TCG Lexicon.
const uint8_t kUidSessionMgmt[8]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF};
const uint8_t kUidThisSp[8]       = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
const uint8_t kUidAdminSp[8]      = {0x00, 0x00, 0x02, 0x05, 0x00, 0x00, 0x00, 0x01};
const uint8_t kUidPsid[8]         = {0x00, 0x00, 0x00, 0x09, 0x00, 0x01, 0xFF, 0x01};
const uint8_t kMStartSession[8]   = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x02};
const uint8_t kMAuthenticate[8]   = {0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x0C};
const uint8_t kMRevert[8]         = {0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x02};

struct TokenWriter {
    std::vector<uint8_t>& out;
    explicit TokenWriter(std::vector<uint8_t>& o) : out(o) {}

    void uid(const uint8_t* u) { out.push_back(tokBytestring8); out.insert(out.end(), u, u + 8); }
    void method(const uint8_t* m) { uid(m); }
    void call(const uint8_t* invoker, const uint8_t* m) {
        out.push_back(tokCall);
        uid(invoker);
        method(m);
    }
    void smallUint(uint8_t v) { out.push_back(v); }  // tiny atom: value < 8
    void uint(uint64_t v) {
        if (v < 8) { out.push_back(uint8_t(v)); return; }
        if (v <= 0xFF) { out.push_back(0x81); out.push_back(uint8_t(v)); }
        else if (v <= 0xFFFF) { out.push_back(0x82); putBe16(push(2), uint16_t(v)); }
        else if (v <= 0xFFFFFFFFull) { out.push_back(0x84); putBe32(push(4), uint32_t(v)); }
        else {
            out.push_back(0x88);
            uint8_t* p = push(8);
            for (int i = 7; i >= 0; --i) p[7 - i] = uint8_t(v >> (i * 8));
        }
    }
    void bytes(const std::string& s) {
        const size_t len = s.size();
        if (len == 0) { out.push_back(0xA1); out.push_back(0x00); return; }
        if (len < 16) { out.push_back(uint8_t(0xA0 | len)); }
        else if (len < 2048) { out.push_back(uint8_t(0xD0 | ((len >> 8) & 0x07))); out.push_back(uint8_t(len & 0xFF)); }
        else return;  // oversized payloads are not supported by this transport
        out.insert(out.end(), s.begin(), s.end());
    }
    void startList() { out.push_back(tokStartList); }
    void endList() { out.push_back(tokEndList); }
    void startName() { out.push_back(tokStartName); }
    void endName() { out.push_back(tokEndName); }
    void namedUint(uint8_t name, uint64_t v) { startName(); smallUint(name); uint(v); endName(); }
    void namedBytes(uint8_t name, const std::string& s) { startName(); smallUint(name); bytes(s); endName(); }
    void namedUid(uint8_t name, const uint8_t* u) { startName(); smallUint(name); uid(u); endName(); }

private:
    uint8_t* push(size_t n) {
        size_t at = out.size();
        out.insert(out.end(), n, 0);
        return out.data() + at;
    }
};

// Writes ComPacket/Packet/DataSubPacket; the payload length drives every 'length' field, and
// the whole packet must be a multiple of 4 bytes (the spec's padding rule).
void buildPacketHeader(uint8_t* hdr, uint32_t comId, uint32_t tsn, uint16_t hsn,
                       uint32_t seq, size_t payloadLen) {
    memset(hdr, 0, kPacketHeaderBytes);
    const uint32_t comPacketLen = uint32_t(kPacketHeaderBytes + payloadLen);
    const uint32_t packetLen = uint32_t(24 + 12 + payloadLen);
    putBe16(hdr + 6, uint16_t((comId >> 16) & 0xFFFF));
    putBe16(hdr + 8, uint16_t(comId & 0xFFFF));
    putBe32(hdr + 16, comPacketLen);
    putBe32(hdr + 20, tsn);
    putBe32(hdr + 24, hsn);
    putBe32(hdr + 28, seq);
    putBe32(hdr + 40, packetLen);
    putBe32(hdr + 48, uint32_t(payloadLen));
}

struct ParsedResponse {
    bool ok = false;
    uint16_t status = 0xFFFF;
    uint32_t tperSessionId = 0;
    bool hasTperSessionId = false;
    size_t payloadLen = 0;
};

ParsedResponse parseResponse(const uint8_t* buf, size_t returned) {
    ParsedResponse pr;
    if (returned < kPacketHeaderBytes + 4) return pr;
    const size_t packetLen = getBe32(buf + 40);
    if (packetLen < 36) return pr;
    const size_t payloadLen = packetLen - 36;
    if (kPacketHeaderBytes + payloadLen > returned) return pr;
    const uint8_t* p = buf + kPacketHeaderBytes;
    pr.payloadLen = payloadLen;
    pr.status = getBe16(p);
    pr.ok = pr.status == 0x0000;
    // Success responses for STARTSESSION return the TPer session id as the first token.
    if (payloadLen >= 3 && p[2] == 0x84) {
        pr.tperSessionId = getBe32(p + 3);
        pr.hasTperSessionId = true;
    } else if (payloadLen >= 2 && p[2] == 0x82) {
        pr.tperSessionId = getBe16(p + 3);
        pr.hasTperSessionId = true;
    } else if (payloadLen >= 2 && p[2] < 0x08) {
        pr.tperSessionId = p[2];
        pr.hasTperSessionId = true;
    }
    return pr;
}

std::wstring statusText(uint16_t s) {
    wchar_t buf[96];
    switch (s) {
    case 0x00: return L"TPer: success";
    case 0x01: return L"TPer: list error";
    case 0x02: return L"TPer: overflow";
    case 0x03: return L"TPer: authentication failed";
    case 0x04: return L"TPer: yetersiz kaynak";
    case 0x05: return L"TPer: gecersiz fonksiyon";
    case 0x06: return L"TPer: gecersiz parametre";
    case 0x25: return L"TPer: authorization rejected";
    case 0x26: return L"TPer: guvenlik durumu revert'e izin vermiyor";
    case 0x3C: return L"TPer: ABORT (revert beklenen davranistir)";
    default:
        swprintf(buf, 96, L"TPer status=0x%04X", (unsigned)s);
        return buf;
    }
}

}  // namespace

std::wstring DiscoveryInfo::summary() const {
    if (!responded) return note.empty() ? L"TCG Level 0 Discovery yanit vermedi" : note;
    std::wstring s;
    if (hasOpal) s += L"OPAL SSC";
    else if (hasEnterprise) s += L"Enterprise SSC";
    else if (hasTPer) s += L"TCG Core (Opal degil)";
    if (hasLocking) s += L" | Locking SP";
    if (lockingSupported) s += lockingEnabled ? L" | Kilitleme ETKIN" : L" | Kilitleme tanimli, kapali";
    if (locked) s += L" | KILITLI";
    if (mediaEncryption) s += L" | medya sifreleme donanimi var";
    if (mbrEnabled) s += L" | PBR(MBR) aktif";
    if (mbrDone) s += L" | PBR yuklendi";
    if (baseComId || comIdCount)
        s += L" | COMID=" + core::formatU64(baseComId) + L" adet=" + core::formatU64(comIdCount);
    if (unknownDescriptors)
        s += L" | " + core::formatU64(unknownDescriptors) + L" tanimsiz tanimlayici";
    if (vendorDescriptors)
        s += L" | " + core::formatU64(vendorDescriptors) + L" vendor tanimlayici";
    return s;
}

CmdResult securityProtocolIn(core::Device& dev, uint8_t protocol, uint32_t specific,
                             void* buffer, uint32_t bufferLen, uint32_t timeoutSec) {
    const core::BusKind bus = core::diskinfo::queryBusKind(dev.handle());
    if (bus == core::BusKind::Nvme) {
        uint8_t sqe[kNvmeCmdSize] = {0};
        sqe[0] = 0x82;  // nvme admin SECURITY RECEIVE
        // CDW10 = protocol<<24 | specific<<8, CDW11 = transfer length.
        const uint32_t cdw10 = (uint32_t(protocol) << 24) | (specific << 8);
        sqe[32] = uint8_t(cdw10); sqe[33] = uint8_t(cdw10 >> 8);
        sqe[34] = uint8_t(cdw10 >> 16); sqe[35] = uint8_t(cdw10 >> 24);
        sqe[36] = uint8_t(bufferLen); sqe[37] = uint8_t(bufferLen >> 8);
        sqe[38] = uint8_t(bufferLen >> 16); sqe[39] = uint8_t(bufferLen >> 24);
        return nvme::rawAdminCommand(dev.handle(), sqe, Direction::DataIn, buffer, bufferLen,
                                     timeoutSec);
    }
    if (bus == core::BusKind::Sas || bus == core::BusKind::Iscsi) {
        // SCSI SECURITY PROTOCOL IN (10): A2h | protocol | BE32 protocol-specific | BE32 length
        uint8_t cdb[10] = {0};
        cdb[0] = 0xA2;
        cdb[1] = protocol;
        cdb[2] = uint8_t(specific >> 24); cdb[3] = uint8_t(specific >> 16);
        cdb[4] = uint8_t(specific >> 8);  cdb[5] = uint8_t(specific);
        cdb[6] = uint8_t(bufferLen >> 24); cdb[7] = uint8_t(bufferLen >> 16);
        cdb[8] = uint8_t(bufferLen >> 8);  cdb[9] = uint8_t(bufferLen);
        return scsiCmd(dev.handle(), cdb, 10, Direction::DataIn, buffer, bufferLen, timeoutSec);
    }
    // SATA / USB(SAT) / RAID / virtual: ATA TRUSTED RECEIVE (5Ch).
    ata::Taskfile tf;
    tf.command = 0x5C;
    tf.features = protocol;
    tf.count = uint8_t(bufferLen / 512u);
    tf.lbaLow = uint8_t(specific & 0xFF);
    tf.lbaMid = uint8_t((specific >> 8) & 0xFF);
    return ata::command(dev.handle(), tf, Direction::DataIn, buffer, bufferLen, timeoutSec);
}

CmdResult securityProtocolOut(core::Device& dev, uint8_t protocol, uint32_t specific,
                              const void* buffer, uint32_t bufferLen, uint32_t timeoutSec) {
    const core::BusKind bus = core::diskinfo::queryBusKind(dev.handle());
    void* mutableBuf = const_cast<void*>(buffer);
    if (bus == core::BusKind::Nvme) {
        uint8_t sqe[kNvmeCmdSize] = {0};
        sqe[0] = 0x81;  // nvme admin SECURITY SEND
        const uint32_t cdw10 = (uint32_t(protocol) << 24) | (specific << 8);
        sqe[32] = uint8_t(cdw10); sqe[33] = uint8_t(cdw10 >> 8);
        sqe[34] = uint8_t(cdw10 >> 16); sqe[35] = uint8_t(cdw10 >> 24);
        sqe[36] = uint8_t(bufferLen); sqe[37] = uint8_t(bufferLen >> 8);
        sqe[38] = uint8_t(bufferLen >> 16); sqe[39] = uint8_t(bufferLen >> 24);
        return nvme::rawAdminCommand(dev.handle(), sqe, Direction::DataOut, mutableBuf, bufferLen,
                                     timeoutSec);
    }
    if (bus == core::BusKind::Sas || bus == core::BusKind::Iscsi) {
        uint8_t cdb[10] = {0};
        cdb[0] = 0xB5;
        cdb[1] = protocol;
        cdb[2] = uint8_t(specific >> 24); cdb[3] = uint8_t(specific >> 16);
        cdb[4] = uint8_t(specific >> 8);  cdb[5] = uint8_t(specific);
        cdb[6] = uint8_t(bufferLen >> 24); cdb[7] = uint8_t(bufferLen >> 16);
        cdb[8] = uint8_t(bufferLen >> 8);  cdb[9] = uint8_t(bufferLen);
        return scsiCmd(dev.handle(), cdb, 10, Direction::DataOut, mutableBuf, bufferLen, timeoutSec);
    }
    ata::Taskfile tf;
    tf.command = 0x5E;  // ATA TRUSTED SEND
    tf.features = protocol;
    tf.count = uint8_t(bufferLen / 512u);
    tf.lbaLow = uint8_t(specific & 0xFF);
    tf.lbaMid = uint8_t((specific >> 8) & 0xFF);
    return ata::command(dev.handle(), tf, Direction::DataOut, mutableBuf, bufferLen, timeoutSec);
}

bool discoverLevel0(core::Device& dev, DiscoveryInfo& out) {
    out = DiscoveryInfo{};
    std::vector<uint8_t> raw(kDiscoveryBuffer + 512, 0);
    uint8_t* buf = raw.data() + ((512 - (uintptr_t(raw.data()) % 512)) % 512);

    CmdResult r = securityProtocolIn(dev, protoOpal, 0x0001, buf, kDiscoveryBuffer, 30);
    if (!r.deviceOk) {
        r = securityProtocolIn(dev, protoTcg, 0x0001, buf, kDiscoveryBuffer, 30);
        if (!r.deviceOk) {
            out.note = L"Level 0 Discovery gonderilemedi: " + describeResult(r);
            return false;
        }
    }
    out.responded = true;

    // The discovery buffer is not a TCG packet: it starts with the 48-byte Discovery-0 header,
    // followed by feature descriptors as (BE16 code, byte version, byte length, body).
    const uint32_t headerLen = getBe32(buf);
    size_t pos = 48;
    const size_t end = (headerLen >= 48 && headerLen <= kDiscoveryBuffer) ? headerLen : kDiscoveryBuffer;
    while (pos + 4 <= end) {
        const uint16_t code = getBe16(buf + pos);
        const uint8_t descLen = buf[pos + 3];
        const uint8_t* d = buf + pos;
        switch (code) {
        case fcTPer:
            out.hasTPer = true;
            break;
        case fcLocking: {
            out.hasLocking = true;
            const uint8_t f = d[4];
            out.lockingSupported = (f & 0x01) != 0;
            out.lockingEnabled = (f & 0x02) != 0;
            out.locked = (f & 0x04) != 0;
            out.mediaEncryption = (f & 0x08) != 0;
            out.mbrEnabled = (f & 0x10) != 0;
            out.mbrDone = (f & 0x20) != 0;
            break;
        }
        case fcEnterprise: out.hasEnterprise = true; break;
        case fcOpalV100:
        case fcOpalV200:
            out.hasOpal = true;
            if (descLen >= 4) {
                out.baseComId = getBe16(d + 4);
                out.comIdCount = getBe16(d + 6);
            }
            break;
        default:
            if (code >= 0xB000) out.vendorDescriptors++;
            else out.unknownDescriptors++;
            break;
        }
        pos += size_t(descLen) + 4;
    }
    return true;
}

RevertResult psidRevert(core::Device& dev, const std::wstring& psid) {
    RevertResult res;
    std::string secret = core::narrowAnsi(psid);
    if (secret.empty() || secret.size() > 64) {
        res.detail = L"PSID 1-64 karakter olmali (etiketinde yazan deger, bosluk/bas lik yok).";
        return res;
    }
    res.attempted = true;

    // Admin SP lives on COMID 0; the Opal descriptors report the *Locking* SP base COMID.
    const uint32_t comId = 0x0000;
    std::vector<uint8_t> buf(2048 + 512, 0);
    uint8_t* packet = buf.data() + ((512 - (uintptr_t(buf.data()) % 512)) % 512);

    auto exchange = [&](const std::vector<uint8_t>& payload, uint32_t tsn, uint32_t seq,
                        ParsedResponse& parsed, CmdResult& transport) -> bool {
        std::vector<uint8_t> padded = payload;
        while (padded.size() % 4) padded.push_back(0);  // spec requires mod-4 packets
        buildPacketHeader(packet, comId, tsn, kHostSessionId, seq, padded.size());
        memcpy(packet + kPacketHeaderBytes, padded.data(), padded.size());
        const uint32_t outLen = uint32_t(kPacketHeaderBytes + padded.size());
        transport = securityProtocolOut(dev, protoOpal, comId, packet, outLen, 60);
        if (!transport.deviceOk) {
            parsed.ok = false;
            return false;
        }
        transport = securityProtocolIn(dev, protoOpal, comId, packet, 2048, 60);
        if (!transport.deviceOk) return false;
        parsed = parseResponse(packet, 2048);
        return parsed.ok;
    };

    // 1. StartSession(Admin SP, write, single-session)
    std::vector<uint8_t> payload;
    TokenWriter tw(payload);
    tw.call(kUidSessionMgmt, kMStartSession);
    tw.startList();
    tw.uint(kHostSessionId);
    tw.uid(kUidAdminSp);
    tw.smallUint(0x01);  // Session type / write access
    tw.startName();
    tw.smallUint(0x00);
    tw.bytes(secret);
    tw.endName();
    tw.startName();
    tw.smallUint(0x03);
    tw.uid(kUidPsid);
    tw.endName();
    tw.endList();

    ParsedResponse start;
    CmdResult tr;
    if (!exchange(payload, 0, 0, start, tr)) {
        res.detail = L"StartSession basarisiz: " + describeResult(tr);
        return res;
    }
    res.sessionStarted = true;
    if (!start.hasTperSessionId) {
        res.detail = L"StartSession TPer session ID dondurmedi; revert durduruldu.";
        return res;
    }
    const uint32_t tsn = start.tperSessionId;

    // 2. Authenticate(PSID authority, secret as entered - PSIDs are never hashed)
    payload.clear();
    tw.call(kUidThisSp, kMAuthenticate);
    tw.startList();
    tw.uid(kUidPsid);
    tw.namedBytes(0x00, secret);
    tw.endList();
    ParsedResponse auth;
    if (!exchange(payload, tsn, 1, auth, tr)) {
        res.statusCode = auth.status;
        res.detail = L"Authenticate(PSID) basarisiz: " + statusText(auth.status) + L" " + describeResult(tr);
        return res;
    }
    res.authenticated = true;

    // 3. Revert(Admin SP) with an empty parameter list. The TPer answers with an abort and the
    //    session then disappears - that abort is the expected outcome, not a failure.
    payload.clear();
    tw.call(kUidAdminSp, kMRevert);
    tw.startList();
    tw.endList();
    ParsedResponse rev;
    exchange(payload, tsn, 2, rev, tr);
    res.statusCode = rev.status;
    res.revertAccepted = rev.ok || rev.status == 0x3C;
    res.detail = rev.ok ? L"Revert kabul edildi. PSID artik geberli degil ve surucu fabrika "
                        L"durumundadir."
              : res.revertAccepted ? L"Revert islemi TPer tarafindan abort ile sonlandirildi "
                                    L"(beklenen davranis)."
                                   : L"Revert reddedildi: " + statusText(rev.status);
    return res;
}

}  // namespace tcg
}  // namespace hw
