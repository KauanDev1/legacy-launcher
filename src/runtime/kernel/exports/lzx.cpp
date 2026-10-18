// SPDX-License-Identifier: MIT
// LDI* imports (the LZX decoder behind XMemDecompress) on libmspack's lzxd.c, which is already linked through
// libXenonUtils.a (LGPL-2.1, ADR 0002).
//
// Xenia declares the four exports (xboxkrnl ordinals 0xB6, 0xB7, 0xB8, 0x2E2) but has no implementation, so there
// is no reference to confirm against; the layout below is the public Microsoft LDI/MDI one that the Xbox 360
// xcompress library uses (see docs/reverse-engineering/kernel-imports.md, section 12):
//   LDICreateDecompression(u32* max_block_out, LZX_DECOMPRESS* config {be u32 window_size (bytes), be u32 cpu_type},
//                          alloc_fn, free_fn, u32* src_buffer_min_out, handle* ctx_out, ...) -> 0 or an LDI error.
//   LDIDecompress(ctx, src, src_size, dst, u32* dst_size) -> 0 or an LDI error; *dst_size is the number of bytes this
//                          block must produce on entry (the frame header of XMemDecompress carries it) and the
//                          produced count on return.
//   LDIResetDecompression(ctx), LDIDestroyDecompression(ctx).
// Deviations: the guest alloc/free callbacks are ignored (lzxd state lives on the host heap); the context is a
// KObject handle in the ObjectTable, so NtClose also frees it. Every call's input starts on a fresh 16-bit word:
// leftover read-ahead from the previous block is dropped, as each block is a separate buffer in LDI.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

extern "C" {
#include "mspack.h"
#include "lzx.h"
}

#include "runtime/be.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/marshal.h"
#include "runtime/kernel/objects.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/status.h"


namespace lr::kernel {
namespace {

using U32Ptr = GuestPtr<be<uint32_t>>;

// A bounded in-memory cursor; libmspack only sees it as an opaque mspack_file* (reinterpret_cast).
struct MemFile {
  uint8_t* data = nullptr;
  size_t size = 0;
  size_t pos = 0;
};
MemFile* mem(mspack_file* f) { return reinterpret_cast<MemFile*>(f); }

// LDI error codes (Microsoft MDI_ERROR_*).
constexpr uint32_t kLdiOk = 0;
constexpr uint32_t kLdiNoMemory = 1;
constexpr uint32_t kLdiBadParameters = 2;
constexpr uint32_t kLdiBufferOverflow = 3;
constexpr uint32_t kLdiFailed = 4;

constexpr uint32_t kMaxBlock = 32768;                 // LZX frame size: the largest block LDIDecompress produces
constexpr uint32_t kSrcBufferMin = kMaxBlock + 6144;  // worst-case compressed block, as in the MS API
constexpr uint32_t kMinWindowBits = 15, kMaxWindowBits = 21;

struct LzxConfig {
  be<uint32_t> window_size;
  be<uint32_t> cpu_type;
};

int ms_read(mspack_file* mf, void* buf, int bytes) {
  MemFile* f = mem(mf);
  if (bytes < 0) return -1;
  const size_t n = std::min(static_cast<size_t>(bytes), f->size - f->pos);
  if (n != 0) std::memcpy(buf, f->data + f->pos, n);
  f->pos += n;
  return static_cast<int>(n);
}
int ms_write(mspack_file* mf, void* buf, int bytes) {
  MemFile* f = mem(mf);
  if (bytes < 0 || static_cast<size_t>(bytes) > f->size - f->pos) return -1;  // never past the destination
  if (bytes != 0) std::memcpy(f->data + f->pos, buf, static_cast<size_t>(bytes));
  f->pos += static_cast<size_t>(bytes);
  return bytes;
}
void* ms_alloc(mspack_system*, size_t bytes) { return std::malloc(bytes); }
void ms_free(void* p) { std::free(p); }
void ms_copy(void* src, void* dst, size_t bytes) { std::memcpy(dst, src, bytes); }
void ms_message(mspack_file*, const char*, ...) {}

mspack_system make_system() {
  mspack_system s{};
  s.read = ms_read;
  s.write = ms_write;
  s.alloc = ms_alloc;
  s.free = ms_free;
  s.copy = ms_copy;
  s.message = ms_message;
  return s;
}

class LzxContext final : public KObject {
 public:
  explicit LzxContext(int window_bits) : KObject(ObjectKind::Other), window_bits_(window_bits), sys_(make_system()) {}
  ~LzxContext() override {
    if (lzx_ != nullptr) lzxd_free(lzx_);
  }
  bool reset() {
    std::lock_guard lock(mutex_);
    if (lzx_ != nullptr) lzxd_free(lzx_);
    lzx_ = lzxd_init(&sys_, reinterpret_cast<mspack_file*>(&in_), reinterpret_cast<mspack_file*>(&out_), window_bits_, 0, static_cast<int>(kSrcBufferMin), 0, 0);
    return lzx_ != nullptr;
  }
  // Decodes one block, `want` bytes into dst. Returns an LDI code; *produced is set on success.
  uint32_t decompress(uint8_t* src, size_t src_size, uint8_t* dst, uint32_t want, uint32_t* produced) {
    std::lock_guard lock(mutex_);
    if (lzx_ == nullptr) return kLdiFailed;
    in_ = {src, src_size, 0};
    out_ = {dst, want, 0};
    lzx_->i_ptr = lzx_->i_end;  // drop read-ahead of the previous block, start on a fresh word
    lzx_->bit_buffer = 0;
    lzx_->bits_left = 0;
    lzx_->input_end = 0;
    lzxd_set_output_length(lzx_, lzx_->offset + static_cast<off_t>(want));
    const int err = lzxd_decompress(lzx_, static_cast<off_t>(want));
    *produced = static_cast<uint32_t>(out_.pos);
    in_ = {};
    out_ = {};
    return err == MSPACK_ERR_OK ? kLdiOk : kLdiFailed;
  }

 private:
  std::mutex mutex_;
  const int window_bits_;
  mspack_system sys_;
  MemFile in_, out_;
  lzxd_stream* lzx_ = nullptr;
};

std::shared_ptr<LzxContext> resolve(uint32_t handle) {
  return std::dynamic_pointer_cast<LzxContext>(kernel().objects().get(handle));
}

}  // namespace

namespace exports {

uint32_t LDICreateDecompression(GuestCall& c, U32Ptr max_block_out, GuestPtr<LzxConfig> config, uint32_t /*alloc*/,
                                uint32_t /*free*/, U32Ptr src_min_out, U32Ptr handle_out) {
  if (!config.valid_range(sizeof(LzxConfig)) || !handle_out.valid_range(4)) return kLdiBadParameters;
  const uint32_t window = config.get(c.base)->window_size;
  uint32_t bits = 0;
  while (bits < 32 && (uint64_t{1} << bits) < window) ++bits;
  if (bits < kMinWindowBits || bits > kMaxWindowBits || (uint64_t{1} << bits) != window) return kLdiBadParameters;
  auto ctx = std::make_shared<LzxContext>(static_cast<int>(bits));
  if (!ctx->reset()) return kLdiNoMemory;
  if (max_block_out.valid_range(4)) *max_block_out.get(c.base) = kMaxBlock;
  if (src_min_out.valid_range(4)) *src_min_out.get(c.base) = kSrcBufferMin;
  *handle_out.get(c.base) = kernel().objects().add(std::move(ctx));
  return kLdiOk;
}

uint32_t LDIDecompress(GuestCall& c, uint32_t handle, GuestPtr<uint8_t> src, uint32_t src_size,
                       GuestPtr<uint8_t> dst, U32Ptr dst_size) {
  auto ctx = resolve(handle);
  if (!ctx || !dst_size.valid_range(4)) return kLdiBadParameters;
  const uint32_t want = *dst_size.get(c.base);
  if (!dst.valid_range(want) || (src_size != 0 && !src.valid_range(src_size))) return kLdiBadParameters;
  if (want > kMaxBlock) return kLdiBufferOverflow;
  uint32_t produced = 0;
  const uint32_t r = ctx->decompress(src.get(c.base), src_size, dst.get(c.base), want, &produced);
  if (r == kLdiOk) *dst_size.get(c.base) = produced;
  return r;
}

uint32_t LDIResetDecompression(uint32_t handle) {
  auto ctx = resolve(handle);
  if (!ctx) return kLdiBadParameters;
  return ctx->reset() ? kLdiOk : kLdiNoMemory;
}

uint32_t LDIDestroyDecompression(uint32_t handle) {
  if (!resolve(handle)) return kLdiBadParameters;
  return kernel().objects().close(handle) == STATUS_SUCCESS ? kLdiOk : kLdiBadParameters;
}

}  // namespace exports

LR_KERNEL(LDICreateDecompression);
LR_KERNEL(LDIDecompress);
LR_KERNEL(LDIResetDecompression);
LR_KERNEL(LDIDestroyDecompression);

}  // namespace lr::kernel
