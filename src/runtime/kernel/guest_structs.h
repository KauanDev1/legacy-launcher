// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

#include "runtime/be.h"

namespace lr::kernel {

using lr::runtime::be;

// Guest-memory layouts (all multi-byte fields big-endian). Offsets follow the Xbox 360 kernel as
// documented by Xenia (src/xenia/kernel/xthread.h).

struct X_DISPATCHER_HEADER {
  uint8_t type;
  uint8_t absolute;
  uint8_t size;
  uint8_t inserted;
  be<int32_t> signal_state;
  be<uint32_t> wait_list_flink;
  be<uint32_t> wait_list_blink;
};
static_assert(sizeof(X_DISPATCHER_HEADER) == 0x10);
static_assert(offsetof(X_DISPATCHER_HEADER, signal_state) == 4);
static_assert(offsetof(X_DISPATCHER_HEADER, wait_list_flink) == 8);
static_assert(offsetof(X_DISPATCHER_HEADER, wait_list_blink) == 0xC);

struct X_KEVENT {
  X_DISPATCHER_HEADER header;
};
static_assert(sizeof(X_KEVENT) == 0x10);

struct X_KSEMAPHORE {
  X_DISPATCHER_HEADER header;
  be<int32_t> limit;
};
static_assert(sizeof(X_KSEMAPHORE) == 0x14);
static_assert(offsetof(X_KSEMAPHORE, limit) == 0x10);

struct X_KMUTANT {
  X_DISPATCHER_HEADER header;
  uint8_t pad_10[8];
  be<uint32_t> owner;
  be<uint32_t> abandoned;
};
static_assert(sizeof(X_KMUTANT) == 0x20);
static_assert(offsetof(X_KMUTANT, owner) == 0x18);
static_assert(offsetof(X_KMUTANT, abandoned) == 0x1C);

struct X_RTL_CRITICAL_SECTION {
  X_DISPATCHER_HEADER header;
  be<int32_t> lock_count;
  be<int32_t> recursion_count;
  be<uint32_t> owning_thread;
};
static_assert(sizeof(X_RTL_CRITICAL_SECTION) == 0x1C);
static_assert(offsetof(X_RTL_CRITICAL_SECTION, lock_count) == 0x10);
static_assert(offsetof(X_RTL_CRITICAL_SECTION, recursion_count) == 0x14);
static_assert(offsetof(X_RTL_CRITICAL_SECTION, owning_thread) == 0x18);

// Only the fields we use are named. Offsets match Xenia's X_KPCR (tls_ptr 0x00, pcr_ptr 0x30,
// stack_base_ptr 0x70, stack_end_ptr 0x74, current_thread 0x100, current_cpu 0x10C). Size 0x2D8 is from
// the brief (Xenia declares no sizeof).
struct X_KPCR {
  be<uint32_t> tls_ptr;  // 0x00
  uint8_t pad_04[0x2C];
  be<uint32_t> pcr_ptr;  // 0x30
  uint8_t pad_34[0x3C];
  be<uint32_t> stack_base;  // 0x70
  be<uint32_t> stack_end;   // 0x74
  uint8_t pad_78[0x88];
  be<uint32_t> current_thread;  // 0x100
  uint8_t pad_104[8];
  uint8_t current_cpu;  // 0x10C
  uint8_t pad_10d[0x2D8 - 0x10D];
};
static_assert(sizeof(X_KPCR) == 0x2D8);
static_assert(offsetof(X_KPCR, pcr_ptr) == 0x30);
static_assert(offsetof(X_KPCR, stack_base) == 0x70);
static_assert(offsetof(X_KPCR, stack_end) == 0x74);
static_assert(offsetof(X_KPCR, current_thread) == 0x100);
static_assert(offsetof(X_KPCR, current_cpu) == 0x10C);

// Partial: only the fields we use. Verified against Xenia xthread.h X_KTHREAD: stack_base 0x5C,
// stack_limit 0x60, tls_address 0x68, thread_id 0x14C, start_address 0x150, last_error 0x160. Xenia's
// full size is 0xAB0; the rest is padding here.
struct X_KTHREAD {
  X_DISPATCHER_HEADER header;  // type 6
  uint8_t pad_10[0x4C];
  be<uint32_t> stack_base;   // 0x5C
  be<uint32_t> stack_limit;  // 0x60
  uint8_t pad_64[4];
  be<uint32_t> tls_address;  // 0x68
  uint8_t pad_6c[0x14C - 0x6C];
  be<uint32_t> thread_id;      // 0x14C
  be<uint32_t> start_address;  // 0x150
  uint8_t pad_154[0x160 - 0x154];
  be<uint32_t> last_error;  // 0x160
  uint8_t pad_164[0xAB0 - 0x164];
};
static_assert(sizeof(X_KTHREAD) == 0xAB0);
static_assert(offsetof(X_KTHREAD, stack_base) == 0x5C);
static_assert(offsetof(X_KTHREAD, stack_limit) == 0x60);
static_assert(offsetof(X_KTHREAD, tls_address) == 0x68);
static_assert(offsetof(X_KTHREAD, thread_id) == 0x14C);
static_assert(offsetof(X_KTHREAD, start_address) == 0x150);
static_assert(offsetof(X_KTHREAD, last_error) == 0x160);

struct X_ANSI_STRING {
  be<uint16_t> length;
  be<uint16_t> maximum_length;
  be<uint32_t> buffer;
};
static_assert(sizeof(X_ANSI_STRING) == 8);
static_assert(offsetof(X_ANSI_STRING, buffer) == 4);

struct X_OBJECT_ATTRIBUTES {
  be<uint32_t> root_directory;
  be<uint32_t> name_ptr;
  be<uint32_t> attributes;
};
static_assert(sizeof(X_OBJECT_ATTRIBUTES) == 0xC);
static_assert(offsetof(X_OBJECT_ATTRIBUTES, name_ptr) == 4);
static_assert(offsetof(X_OBJECT_ATTRIBUTES, attributes) == 8);

struct X_IO_STATUS_BLOCK {
  be<uint32_t> status;
  be<uint32_t> information;
};
static_assert(sizeof(X_IO_STATUS_BLOCK) == 8);

struct X_LARGE_INTEGER {
  be<int64_t> quad_part;
};
static_assert(sizeof(X_LARGE_INTEGER) == 8);

// NtQueryVirtualMemory output. Field order from Xenia xboxkrnl_memory.cc (NtQueryVirtualMemory fills base_address,
// allocation_base, allocation_protect, region_size, state, protect, type), all u32.
struct X_MEMORY_BASIC_INFORMATION {
  be<uint32_t> base_address;
  be<uint32_t> allocation_base;
  be<uint32_t> allocation_protect;
  be<uint32_t> region_size;
  be<uint32_t> state;
  be<uint32_t> protect;
  be<uint32_t> type;
};
static_assert(sizeof(X_MEMORY_BASIC_INFORMATION) == 0x1C);
static_assert(offsetof(X_MEMORY_BASIC_INFORMATION, allocation_protect) == 8);
static_assert(offsetof(X_MEMORY_BASIC_INFORMATION, region_size) == 0xC);
static_assert(offsetof(X_MEMORY_BASIC_INFORMATION, type) == 0x18);

// MmQueryStatistics output, 104 bytes (static-asserted in Xenia xboxkrnl_memory.cc, X_MM_QUERY_STATISTICS_RESULT).
struct X_MM_QUERY_STATISTICS_SECTION {
  be<uint32_t> available_pages;
  be<uint32_t> total_virtual_memory_bytes;
  be<uint32_t> reserved_virtual_memory_bytes;
  be<uint32_t> physical_pages;
  be<uint32_t> pool_pages;
  be<uint32_t> stack_pages;
  be<uint32_t> image_pages;
  be<uint32_t> heap_pages;
  be<uint32_t> virtual_pages;
  be<uint32_t> page_table_pages;
  be<uint32_t> cache_pages;
};
static_assert(sizeof(X_MM_QUERY_STATISTICS_SECTION) == 0x2C);

struct X_MM_QUERY_STATISTICS_RESULT {
  be<uint32_t> size;
  be<uint32_t> total_physical_pages;
  be<uint32_t> kernel_pages;
  X_MM_QUERY_STATISTICS_SECTION title;
  X_MM_QUERY_STATISTICS_SECTION system;
  be<uint32_t> highest_physical_page;
};
static_assert(sizeof(X_MM_QUERY_STATISTICS_RESULT) == 104);
static_assert(offsetof(X_MM_QUERY_STATISTICS_RESULT, title) == 0xC);
static_assert(offsetof(X_MM_QUERY_STATISTICS_RESULT, system) == 0x38);
static_assert(offsetof(X_MM_QUERY_STATISTICS_RESULT, highest_physical_page) == 0x64);

}  // namespace lr::kernel
