// SPDX-License-Identifier: MIT
// Network startup probes: M2 is offline, the network libraries only need their startup calls to succeed and to see
// no link. Every NetDll_* import takes the calling module (caller) in r3 before its Winsock/XNet arguments (Xenia's
// xam_net.cc; semantic reference only, no code copied). The rest of NetDll_* stays missing until the trace shows it.
#include <cstdint>
#include <cstring>

#include "runtime/be.h"
#include "runtime/kernel/marshal.h"
#include "runtime/kernel/registry.h"

namespace lr::kernel {
using lr::runtime::be;

namespace exports {

// NetDll_WSAStartup(caller, version, WSADATA*): 0, filling WSADATA like Xenia's non-Windows path: the requested
// version, empty description/status, 100 sockets, 1024-byte datagrams. X_WSADATA: version +0, version_high +2,
// description[257] +4, system_status[129] +0x105, max_sockets +0x186, max_udpdg +0x188 (vendor_info untouched).
uint32_t NetDll_WSAStartup(GuestCall& c, uint32_t /*caller*/, uint16_t version, GuestPtr<uint8_t> data) {
  if (data.valid_range(0x18A)) {
    uint8_t* p = data.get(c.base);
    *reinterpret_cast<be<uint16_t>*>(p + 0x000) = version;
    *reinterpret_cast<be<uint16_t>*>(p + 0x002) = version;
    std::memset(p + 0x004, 0, 257 + 129);
    *reinterpret_cast<be<uint16_t>*>(p + 0x186) = 100;
    *reinterpret_cast<be<uint16_t>*>(p + 0x188) = 1024;
  }
  return 0;
}

// NetDll_XNetGetTitleXnAddr / NetDll_XNetGetDebugXnAddr(caller, XNADDR*): offline, so the 36-byte XNADDR is zeroed
// and the answer is XNET_GET_XNADDR_NONE (1), a final state (PENDING, 0, would make the game poll).
constexpr uint32_t kXnAddrBytes = 36;
constexpr uint32_t kXnAddrNone = 1;
uint32_t NetDll_XNetGetTitleXnAddr(GuestCall& c, uint32_t /*caller*/, GuestPtr<uint8_t> addr) {
  if (addr.valid_range(kXnAddrBytes)) std::memset(addr.get(c.base), 0, kXnAddrBytes);
  return kXnAddrNone;
}
uint32_t NetDll_XNetGetDebugXnAddr(GuestCall& c, uint32_t caller, GuestPtr<uint8_t> addr) {
  return NetDll_XNetGetTitleXnAddr(c, caller, addr);
}

}  // namespace exports

LR_KERNEL_STUB_FN(NetDll_WSAStartup);                 // Winsock "started": offline, nothing to initialize
LR_KERNEL_STUB(NetDll_XNetStartup, 0);                // XNet "started": offline, nothing to initialize
LR_KERNEL_STUB(NetDll_XNetGetEthernetLinkStatus, 0);  // 0 = no Ethernet link: M2 is offline
LR_KERNEL_STUB_FN(NetDll_XNetGetTitleXnAddr);         // offline: zeroed address, XNET_GET_XNADDR_NONE
LR_KERNEL_STUB_FN(NetDll_XNetGetDebugXnAddr);         // same

}  // namespace lr::kernel
