// SPDX-License-Identifier: MIT
// PM4 command processor: decodes the packet stream the guest writes into its ring buffer and indirect buffers,
// applies register writes to the RegisterFile, writes fences into guest memory and drives a Backend. Packet
// semantics per Xenia command_processor.cc / xenos.h (reference only; no code taken from it).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>

#include "common/result.h"
#include "runtime/gpu/backend.h"
#include "runtime/gpu/registers.h"

namespace lr::gpu {

// How the command processor reaches guest memory. Guest memory is big-endian; base points at guest address 0.
struct MemoryAccess {
  uint8_t* base;
  // Maps a physical address taken from a packet to a guest address; nullopt when it maps to no guest memory.
  // The processor masks every packet address to the 512 MiB physical space (phys & 0x1FFFFFFF, so 0xA0xxxxxx /
  // 0xC0xxxxxx / 0xE0xxxxxx view-tagged addresses work) before calling it. A 4-byte fence/poll access whose
  // translated address is not 4-byte aligned is malformed.
  std::function<std::optional<uint32_t>(uint32_t phys)> to_guest;
  // Bytes of guest memory reachable from base (the runtime maps the whole 4 GiB space, GuestMemory::size()).
  // Every access is checked against it: [guest, guest + n) must lie in [0, size). Interface addition (Task 8).
  uint64_t size = 0;
};

namespace pm4 {

inline constexpr uint32_t kType0 = 0, kType1 = 1, kType2 = 2, kType3 = 3;

// Type-3 opcodes (header bits 8-14). Numbers per Xenia xenos.h (reference only; they are hardware facts).
enum Opcode : uint32_t {
  NOP = 0x10,
  REG_RMW = 0x21,
  DRAW_INDX = 0x22,
  VIZ_QUERY = 0x23,
  WAIT_FOR_IDLE = 0x26,
  IM_LOAD = 0x27,
  IM_LOAD_IMMEDIATE = 0x2B,
  SET_CONSTANT = 0x2D,
  LOAD_ALU_CONSTANT = 0x2F,
  DRAW_INDX_2 = 0x36,
  INDIRECT_BUFFER_PFD = 0x37,
  INVALIDATE_STATE = 0x3B,
  WAIT_REG_MEM = 0x3C,
  MEM_WRITE = 0x3D,
  REG_TO_MEM = 0x3E,
  INDIRECT_BUFFER = 0x3F,
  COND_WRITE = 0x45,
  EVENT_WRITE = 0x46,
  ME_INIT = 0x48,
  SET_BIN_BASE_OFFSET = 0x4B,
  SET_BIN_MASK = 0x50,
  SET_BIN_SELECT = 0x51,
  INTERRUPT = 0x54,
  SET_CONSTANT2 = 0x55,
  SET_SHADER_CONSTANTS = 0x56,
  EVENT_WRITE_SHD = 0x58,
  EVENT_WRITE_EXT = 0x5A,
  EVENT_WRITE_ZPD = 0x5B,
  CONTEXT_UPDATE = 0x5E,
  SET_BIN_MASK_LO = 0x60,
  SET_BIN_MASK_HI = 0x61,
  SET_BIN_SELECT_LO = 0x62,
  SET_BIN_SELECT_HI = 0x63,
  // Not a hardware opcode: our HLE VdSwap emits it. Payload: kSwapMagic, frontbuffer physical address, width,
  // height; any further dwords are ignored (VdSwap fills the game's reserved area). Without the magic the packet
  // is malformed. Calls Backend::swap and advances the swap counter EVENT_WRITE_SHD reports.
  LR_SWAP = 0x64,
};

inline constexpr uint32_t kSwapMagic = 0x4C525357;  // 'LRSW'
inline constexpr uint32_t kPhysicalMask = 0x1FFFFFFF;  // 512 MiB physical address space

// Name of a known opcode ("DRAW_INDX"), nullptr otherwise.
const char* opcode_name(uint32_t opcode);

// Endian modes in the low 2 bits of fence/poll addresses (xenos Endian).
inline constexpr uint32_t kEndianNone = 0, kEndian8in16 = 1, kEndian8in32 = 2, kEndian16in32 = 3;
uint32_t gpu_swap(uint32_t value, uint32_t endian);

inline constexpr uint32_t kMaxIndirectDepth = 4;  // nested INDIRECT_BUFFER levels below the ring buffer

}  // namespace pm4

// Thread-safety: execute() and opcode_counts() belong to the GPU thread (or run after it stopped);
// request_stop() may be called from any thread.
class CommandProcessor {
 public:
  CommandProcessor(MemoryAccess mem, RegisterFile& regs, Backend& backend,
                   std::function<void(uint32_t source)> raise_interrupt);

  // Executes the packet dwords in [guest_addr, guest_addr + 4 * dwords) (4-byte aligned, the top-level ring
  // buffer). Returns the dwords consumed: all of them, or fewer when the last packet is incomplete (its header or
  // payload runs past `dwords`) - that packet is not executed, so a ring reader resumes at guest_addr + 4 *
  // consumed once the rest (e.g. after the wrap) is available. Fatal errors: unknown type-3 opcode ("unknown PM4
  // opcode 0x.."), a packet too short for its opcode, a packet split across the end of an INDIRECT_BUFFER, an
  // address outside guest memory, indirect buffers nested deeper than pm4::kMaxIndirectDepth (all InvalidFormat),
  // or Cancelled when request_stop() ended a WAIT_REG_MEM.
  Result<uint32_t> execute(uint32_t guest_addr, uint32_t dwords);

  // Type-3 opcode -> packets seen (including a final unknown one), for --trace-gpu and fatal reports.
  std::map<uint32_t, uint64_t> opcode_counts() const { return counts_; }

  // Escape hatch for shutdown: a WAIT_REG_MEM that is spinning (or the next one) returns Cancelled. Sticky.
  void request_stop() { stop_.store(true, std::memory_order_release); }

  // Frames swapped through LR_SWAP so far (the value EVENT_WRITE_SHD writes when initiator bit 31 is set).
  uint32_t swap_counter() const { return swap_counter_; }

  // Called for every complete packet before it executes (ring and indirect buffers alike): header type (0-3),
  // type-3 opcode (0 for types 0-2) and the packet's size in dwords, header included. An all-zero padding dword is
  // not reported. For --trace-gpu (GpuSystem). Set it before the first execute().
  using PacketObserver = std::function<void(uint32_t type, uint32_t opcode, uint32_t dwords)>;
  void set_packet_observer(PacketObserver observer) { observer_ = std::move(observer); }

 private:
  // Returns the dwords consumed; inside an INDIRECT_BUFFER (depth > 0) an incomplete trailing packet is an error.
  Result<uint32_t> execute_buffer(uint32_t guest_addr, uint32_t dwords, uint32_t depth);
  Result<void> execute_type3(uint32_t opcode, uint32_t payload_addr, uint32_t count, uint32_t depth);

  bool in_bounds(uint32_t guest, uint64_t bytes) const;
  uint32_t read_be(uint32_t guest) const;  // caller checked bounds
  Result<uint32_t> translate(uint32_t phys, uint64_t bytes, const char* what) const;
  Result<uint32_t> load_mem(uint32_t phys_and_endian, const char* what) const;
  Result<void> store_mem(uint32_t phys_and_endian, uint32_t value, const char* what);
  Result<bool> compare(uint32_t wait_info, uint32_t poll, uint32_t ref, uint32_t mask);
  // Every register write goes through here: SCRATCH_REG0..7 writes are also written back to memory when their
  // SCRATCH_UMSK bit is set (a bad SCRATCH_ADDR is a malformed-packet error).
  Result<void> write_register(uint32_t index, uint32_t value);

  MemoryAccess mem_;
  RegisterFile& regs_;
  Backend& backend_;
  std::function<void(uint32_t)> raise_interrupt_;
  PacketObserver observer_;
  std::map<uint32_t, uint64_t> counts_;
  std::atomic<bool> stop_{false};
  uint32_t swap_counter_ = 0;
  uint64_t bin_mask_ = ~uint64_t{0};
  uint64_t bin_select_ = ~uint64_t{0};
};

}  // namespace lr::gpu
