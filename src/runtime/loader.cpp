// SPDX-License-Identifier: MIT
#include "runtime/loader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "formats/xex2.h"
#include "image.h"
#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/kernel.h"

namespace lr::runtime {

namespace {
std::string hex(uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof b, "0x%08llX", static_cast<unsigned long long>(v));
  return b;
}

bool is_xex2(std::span<const std::byte> b) {
  return b.size() >= 4 && b[0] == std::byte{'X'} && b[1] == std::byte{'E'} && b[2] == std::byte{'X'} &&
         b[3] == std::byte{'2'};
}

// Copies the XEX header into the kernel data area (first allocation: kKernelDataBase) and builds the executable's
// LDR entry after it, as the console loader leaves them for RtlImageXexHeaderField / XexExecutableModuleHandle.
Result<void> map_xex_header(GuestMemory& mem, std::span<const std::byte> xex, LoadedImage& out) {
  LR_TRY_ASSIGN(header, formats::parse_xex2(xex));
  const uint32_t header_size = header.pe_data_offset;
  if (header_size < 0x18 || header_size > xex.size())
    return Error{ErrorCode::InvalidFormat, "XEX header size " + hex(header_size) + " is outside the file", ""};
  kernel::Kernel& k = kernel::kernel();
  const uint32_t copy = k.kernel_data_alloc(header_size, 0x1000);
  const uint32_t ldr = k.kernel_data_alloc(sizeof(kernel::X_LDR_DATA_TABLE_ENTRY), 0x10);
  if (copy == 0 || ldr == 0)
    return Error{ErrorCode::InsufficientResources, "kernel data area exhausted mapping the XEX header", ""};
  std::memcpy(mem.base() + copy, xex.data(), header_size);

  auto* e = mem.translate<kernel::X_LDR_DATA_TABLE_ENTRY>(ldr);
  // The only loaded module: every list links back to the entry itself.
  e->in_load_order_links = {ldr, ldr};
  e->in_memory_order_links = {ldr + 0x08, ldr + 0x08};
  e->in_initialization_order_links = {ldr + 0x10, ldr + 0x10};
  e->dll_base = out.base;
  e->image_base = out.base;
  e->image_size = out.size;
  e->full_image_size = out.size;
  e->entry_point = out.entry;
  e->load_count = 1;
  e->dll_base_original = header.image_base.value_or(out.base);
  e->xex_header_base = copy;

  out.header_guest = copy;
  out.ldr_entry = ldr;
  out.tls = header.tls;
  out.stack_size = header.default_stack_size.value_or(kDefaultStackSize);
  return {};
}
}  // namespace

Result<LoadedImage> load_image(GuestMemory& mem, std::span<const std::byte> xex) {
  if (xex.size() < 4) return Error{ErrorCode::InvalidFormat, "image is too small", ""};
  Image image = Image::ParseImage(reinterpret_cast<const uint8_t*>(xex.data()), xex.size());
  if (!image.data || image.sections.empty()) {
    return Error{ErrorCode::InvalidFormat, "XenonUtils could not parse the image", "expected a patched, decrypted XEX2"};
  }
  if (image.base > UINT32_MAX) return Error{ErrorCode::InvalidFormat, "image base does not fit in 32 bits", ""};

  uint64_t end = std::max<uint64_t>(image.base + image.size, image.base);
  for (const Section& s : image.sections) {
    if (s.base < image.base || s.base + s.size > UINT32_MAX) {
      return Error{ErrorCode::InvalidFormat, "section " + s.name + " lies outside the 32-bit image range", ""};
    }
    if (s.base + s.size > mem.size()) {
      return Error{ErrorCode::OutOfRange, "section " + s.name + " at " + hex(s.base) + " does not fit in guest memory", ""};
    }
    end = std::max<uint64_t>(end, s.base + s.size);
  }
  if (image.entry_point < image.base || image.entry_point >= end) {
    return Error{ErrorCode::InvalidFormat, "entry point " + hex(image.entry_point) + " is outside the image", ""};
  }
  if (is_xex2(xex)) {
    // A XEX image is the whole decompressed PE (image.data, image.size bytes at image.base): map all of it, as the
    // console does. The resources (XexGetModuleSection, e.g. the game's XUI package) lie outside every PE section.
    if (image.base + image.size > mem.size()) {
      return Error{ErrorCode::OutOfRange, "image at " + hex(image.base) + " does not fit in guest memory", ""};
    }
    std::memcpy(mem.base() + image.base, image.data.get(), image.size);
  } else {
    // Other images (the test ELF): image.data is the file, so only the sections are mapped.
    for (const Section& s : image.sections) {
      if (s.size && s.data) std::memcpy(mem.base() + s.base, s.data, s.size);
    }
  }
  LoadedImage out;
  out.base = static_cast<uint32_t>(image.base);
  out.size = static_cast<uint32_t>(end - image.base);
  out.entry = static_cast<uint32_t>(image.entry_point);
  if (is_xex2(xex)) LR_TRY(map_xex_header(mem, xex, out));
  return out;
}

}  // namespace lr::runtime
