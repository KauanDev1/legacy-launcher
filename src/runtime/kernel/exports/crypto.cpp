// SPDX-License-Identifier: MIT
// XeCrypt SHA-1 / SHA-256 / random imports. Argument orders and state layouts follow Xenia's xboxkrnl_crypt.cc
// (XECRYPT_SHA_STATE: u32 count, u32 state[5], u8 buffer[64] = 0x58 bytes; XECRYPT_SHA256_STATE: u32 count,
// u32 state[8], u8 buffer[64] = 0x64 bytes, all words big-endian); semantic reference only, no code copied.
//
// All hashing state lives in the guest block (the guest copies and clones states freely); a host Sha1/Sha256 is
// rebuilt from it for each call. `count` is the byte count mod 2^32, which keeps the pending block length exact.
// Deviations: XeCryptSha256Final does not overwrite the state buffer with the digest (Xenia does); it leaves the
// state untouched. XeCryptRandom is real randomness (getrandom), Xenia fills 0xFD. XeCryptRotSumSha (Xenia has no
// implementation, the algorithm is not documented well enough to implement) and XeCryptSha256 (one-shot, not in
// Xenia) stay missing; XeKeys* and XeCryptBnQwBeSigVerify are M4.
#include <sys/random.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "common/sha1.h"
#include "common/sha256.h"
#include "runtime/be.h"
#include "runtime/kernel/marshal.h"
#include "runtime/kernel/registry.h"

namespace lr::kernel {
namespace {

using lr::runtime::be;

struct XeCryptShaState {
  be<uint32_t> count;
  be<uint32_t> state[5];
  uint8_t buffer[64];
};
static_assert(sizeof(XeCryptShaState) == 0x58);
static_assert(offsetof(XeCryptShaState, state) == 4);
static_assert(offsetof(XeCryptShaState, buffer) == 0x18);

struct XeCryptSha256State {
  be<uint32_t> count;
  be<uint32_t> state[8];
  uint8_t buffer[64];
};
static_assert(sizeof(XeCryptSha256State) == 0x64);
static_assert(offsetof(XeCryptSha256State, state) == 4);
static_assert(offsetof(XeCryptSha256State, buffer) == 0x24);

template <class H, size_t N, class S>
H restore(const S& s) {
  uint32_t h[N];
  for (size_t i = 0; i < N; ++i) h[i] = s.state[i];
  uint8_t buf[64];
  std::memcpy(buf, s.buffer, 64);
  H hash;
  hash.load(h, s.count, buf);
  return hash;
}

template <size_t N, class S, class H>
void save(S& s, const H& hash) {
  uint32_t h[N];
  uint8_t buf[64];
  hash.store(h, buf);
  for (size_t i = 0; i < N; ++i) s.state[i] = h[i];
  std::memcpy(s.buffer, buf, 64);
  s.count = static_cast<uint32_t>(hash.total_len());
}

std::span<const std::byte> input_bytes(const GuestCall& c, uint32_t addr, uint32_t size) {
  if (addr == 0 || size == 0 || !GuestPtr<char>{addr}.valid_range(size)) return {};
  return {reinterpret_cast<const std::byte*>(c.base + addr), size};
}

template <size_t N>
void write_digest(const GuestCall& c, uint32_t out, uint32_t out_size, const std::array<uint8_t, N>& d) {
  const uint32_t n = std::min<uint32_t>(out_size, N);
  if (out != 0 && n != 0 && GuestPtr<char>{out}.valid_range(n)) std::memcpy(c.base + out, d.data(), n);
}

template <class S>
S* state_ptr(const GuestCall& c, uint32_t addr) {
  return GuestPtr<S>{addr}.valid_range(sizeof(S)) ? GuestPtr<S>{addr}.get(c.base) : nullptr;
}

}  // namespace

namespace exports {

void XeCryptShaInit(GuestCall& c, uint32_t state) {
  if (auto* s = state_ptr<XeCryptShaState>(c, state)) {
    std::memset(s, 0, sizeof *s);
    save<5>(*s, Sha1{});
  }
}

void XeCryptShaUpdate(GuestCall& c, uint32_t state, uint32_t input, uint32_t size) {
  auto* s = state_ptr<XeCryptShaState>(c, state);
  if (!s) return;
  Sha1 h = restore<Sha1, 5>(*s);
  h.update(input_bytes(c, input, size));
  save<5>(*s, h);
}

void XeCryptShaFinal(GuestCall& c, uint32_t state, uint32_t out, uint32_t out_size) {
  auto* s = state_ptr<XeCryptShaState>(c, state);
  if (!s) return;
  Sha1 h = restore<Sha1, 5>(*s);
  write_digest(c, out, out_size, h.finish());
}

// XeCryptSha(in1, size1, in2, size2, in3, size3, out, out_size): one-shot over up to three buffers.
void XeCryptSha(GuestCall& c, uint32_t in1, uint32_t size1, uint32_t in2, uint32_t size2, uint32_t in3,
                uint32_t size3, uint32_t out, uint32_t out_size) {
  Sha1 h;
  h.update(input_bytes(c, in1, size1));
  h.update(input_bytes(c, in2, size2));
  h.update(input_bytes(c, in3, size3));
  write_digest(c, out, out_size, h.finish());
}

void XeCryptSha256Init(GuestCall& c, uint32_t state) {
  if (auto* s = state_ptr<XeCryptSha256State>(c, state)) {
    std::memset(s, 0, sizeof *s);
    save<8>(*s, Sha256{});
  }
}

void XeCryptSha256Update(GuestCall& c, uint32_t state, uint32_t input, uint32_t size) {
  auto* s = state_ptr<XeCryptSha256State>(c, state);
  if (!s) return;
  Sha256 h = restore<Sha256, 8>(*s);
  h.update(input_bytes(c, input, size));
  save<8>(*s, h);
}

void XeCryptSha256Final(GuestCall& c, uint32_t state, uint32_t out, uint32_t out_size) {
  auto* s = state_ptr<XeCryptSha256State>(c, state);
  if (!s) return;
  Sha256 h = restore<Sha256, 8>(*s);
  write_digest(c, out, out_size, h.finish());
}

void XeCryptRandom(GuestCall& c, uint32_t buffer, uint32_t size) {
  if (buffer == 0 || size == 0 || !GuestPtr<char>{buffer}.valid_range(size)) return;
  uint8_t* p = c.base + buffer;
  uint32_t left = size;
  while (left > 0) {
    const ssize_t n = getrandom(p, left, 0);
    if (n <= 0) break;
    p += n;
    left -= static_cast<uint32_t>(n);
  }
}

}  // namespace exports

LR_KERNEL(XeCryptShaInit);
LR_KERNEL(XeCryptShaUpdate);
LR_KERNEL(XeCryptShaFinal);
LR_KERNEL(XeCryptSha);
LR_KERNEL(XeCryptSha256Init);
LR_KERNEL(XeCryptSha256Update);
LR_KERNEL(XeCryptSha256Final);
LR_KERNEL(XeCryptRandom);

}  // namespace lr::kernel
