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

struct X_UNICODE_STRING {
  be<uint16_t> length;
  be<uint16_t> maximum_length;
  be<uint32_t> buffer;
};
static_assert(sizeof(X_UNICODE_STRING) == 8);
static_assert(offsetof(X_UNICODE_STRING, buffer) == 4);

struct X_LIST_ENTRY {
  be<uint32_t> flink;
  be<uint32_t> blink;
};
static_assert(sizeof(X_LIST_ENTRY) == 8);

// A loaded module; its guest address is the module handle (HMODULE). Field offsets from the comments of Xenia's
// X_LDR_DATA_TABLE_ENTRY (src/xenia/kernel/xmodule.h); Xenia declares no sizeof, 0x64 is the end of its last field.
struct X_LDR_DATA_TABLE_ENTRY {
  X_LIST_ENTRY in_load_order_links;            // 0x00
  X_LIST_ENTRY in_memory_order_links;          // 0x08
  X_LIST_ENTRY in_initialization_order_links;  // 0x10
  be<uint32_t> dll_base;                       // 0x18
  be<uint32_t> image_base;                     // 0x1C
  be<uint32_t> image_size;                     // 0x20
  X_UNICODE_STRING full_dll_name;              // 0x24
  X_UNICODE_STRING base_dll_name;              // 0x2C
  be<uint32_t> flags;                          // 0x34
  be<uint32_t> full_image_size;                // 0x38
  be<uint32_t> entry_point;                    // 0x3C
  be<uint16_t> load_count;                     // 0x40
  be<uint16_t> module_index;                   // 0x42
  be<uint32_t> dll_base_original;              // 0x44
  be<uint32_t> checksum;                       // 0x48 (Xenia keeps its kernel handle here)
  be<uint32_t> load_flags;                     // 0x4C
  be<uint32_t> time_date_stamp;                // 0x50
  be<uint32_t> loaded_imports;                 // 0x54
  be<uint32_t> xex_header_base;                // 0x58: guest copy of the XEX header
  be<uint32_t> closure_root;                   // 0x5C
  be<uint32_t> traversal_parent;               // 0x60
};
static_assert(sizeof(X_LDR_DATA_TABLE_ENTRY) == 0x64);
static_assert(offsetof(X_LDR_DATA_TABLE_ENTRY, dll_base) == 0x18);
static_assert(offsetof(X_LDR_DATA_TABLE_ENTRY, image_size) == 0x20);
static_assert(offsetof(X_LDR_DATA_TABLE_ENTRY, full_dll_name) == 0x24);
static_assert(offsetof(X_LDR_DATA_TABLE_ENTRY, flags) == 0x34);
static_assert(offsetof(X_LDR_DATA_TABLE_ENTRY, entry_point) == 0x3C);
static_assert(offsetof(X_LDR_DATA_TABLE_ENTRY, load_count) == 0x40);
static_assert(offsetof(X_LDR_DATA_TABLE_ENTRY, checksum) == 0x48);
static_assert(offsetof(X_LDR_DATA_TABLE_ENTRY, xex_header_base) == 0x58);
static_assert(offsetof(X_LDR_DATA_TABLE_ENTRY, traversal_parent) == 0x60);

// Kernel object type descriptor, the block behind Ex*ObjectType / IoFileObjectType. Fields from Xenia's
// X_OBJECT_TYPE (src/xenia/kernel/xobject.h): seven u32, pool_tag last.
struct X_OBJECT_TYPE {
  be<uint32_t> constructor;  // 0x00
  be<uint32_t> destructor;   // 0x04
  be<uint32_t> unk_08;
  be<uint32_t> unk_0c;
  be<uint32_t> unk_10;
  be<uint32_t> unk_14;
  be<uint32_t> pool_tag;  // 0x18
};
static_assert(sizeof(X_OBJECT_TYPE) == 0x1C);
static_assert(offsetof(X_OBJECT_TYPE, pool_tag) == 0x18);

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

// --- File information (exports/files.cpp). Times are FILETIME (100 ns since 1601-01-01 UTC). ---

// FileNetworkOpenInformation (class 34) and NtQueryFullAttributesFile output. Xenia src/xenia/kernel/info/file.h
// X_FILE_NETWORK_OPEN_INFORMATION (static_assert size 56).
struct X_FILE_NETWORK_OPEN_INFORMATION {
  be<uint64_t> creation_time;     // 0x00
  be<uint64_t> last_access_time;  // 0x08
  be<uint64_t> last_write_time;   // 0x10
  be<uint64_t> change_time;       // 0x18
  be<uint64_t> allocation_size;   // 0x20
  be<uint64_t> end_of_file;       // 0x28
  be<uint32_t> attributes;        // 0x30
  be<uint32_t> pad;               // 0x34
};
static_assert(sizeof(X_FILE_NETWORK_OPEN_INFORMATION) == 56);
static_assert(offsetof(X_FILE_NETWORK_OPEN_INFORMATION, end_of_file) == 0x28);
static_assert(offsetof(X_FILE_NETWORK_OPEN_INFORMATION, attributes) == 0x30);

// FileStandardInformation (class 5). Xenia declares no struct for it; this is the NT FILE_STANDARD_INFORMATION
// layout (two LARGE_INTEGERs, ULONG links, BOOLEAN delete_pending and directory, padded to 24).
struct X_FILE_STANDARD_INFORMATION {
  be<uint64_t> allocation_size;  // 0x00
  be<uint64_t> end_of_file;      // 0x08
  be<uint32_t> number_of_links;  // 0x10
  uint8_t delete_pending;        // 0x14
  uint8_t directory;             // 0x15
  uint8_t pad[2];
};
static_assert(sizeof(X_FILE_STANDARD_INFORMATION) == 24);
static_assert(offsetof(X_FILE_STANDARD_INFORMATION, number_of_links) == 0x10);
static_assert(offsetof(X_FILE_STANDARD_INFORMATION, directory) == 0x15);

// NtQueryDirectoryFile entry: Xenia src/xenia/kernel/xfile.h X_FILE_DIRECTORY_INFORMATION (field offsets from its
// comments); the ANSI name (file_name_length bytes, no NUL) follows the fixed part at 0x40.
struct X_FILE_DIRECTORY_INFORMATION {
  be<uint32_t> next_entry_offset;  // 0x00
  be<uint32_t> file_index;         // 0x04
  be<uint64_t> creation_time;      // 0x08
  be<uint64_t> last_access_time;   // 0x10
  be<uint64_t> last_write_time;    // 0x18
  be<uint64_t> change_time;        // 0x20
  be<uint64_t> end_of_file;        // 0x28
  be<uint64_t> allocation_size;    // 0x30
  be<uint32_t> attributes;         // 0x38
  be<uint32_t> file_name_length;   // 0x3C
};
static_assert(sizeof(X_FILE_DIRECTORY_INFORMATION) == 0x40);
static_assert(offsetof(X_FILE_DIRECTORY_INFORMATION, end_of_file) == 0x28);
static_assert(offsetof(X_FILE_DIRECTORY_INFORMATION, attributes) == 0x38);
static_assert(offsetof(X_FILE_DIRECTORY_INFORMATION, file_name_length) == 0x3C);

// FileFsSizeInformation (volume class 3): NT FILE_FS_SIZE_INFORMATION; Xenia copies the four device values in this
// order (xboxkrnl_io_info.cc).
struct X_FILE_FS_SIZE_INFORMATION {
  be<uint64_t> total_allocation_units;      // 0x00
  be<uint64_t> available_allocation_units;  // 0x08
  be<uint32_t> sectors_per_allocation_unit; // 0x10
  be<uint32_t> bytes_per_sector;            // 0x14
};
static_assert(sizeof(X_FILE_FS_SIZE_INFORMATION) == 24);
static_assert(offsetof(X_FILE_FS_SIZE_INFORMATION, bytes_per_sector) == 0x14);

// FileFsAttributeInformation (volume class 5): fixed part; the ANSI file system name follows at 12 (Xenia checks
// info_length >= 12 + name size).
struct X_FILE_FS_ATTRIBUTE_INFORMATION {
  be<uint32_t> attributes;                 // 0x00
  be<uint32_t> component_name_max_length;  // 0x04
  be<uint32_t> name_length;                // 0x08
};
static_assert(sizeof(X_FILE_FS_ATTRIBUTE_INFORMATION) == 12);

struct X_LARGE_INTEGER {
  be<int64_t> quad_part;
};
static_assert(sizeof(X_LARGE_INTEGER) == 8);

// Timer body created by NtCreateTimer (type 8 notification / 9 synchronization). Xenia keeps timers host-side and
// declares no X_KTIMER; this is the NT KTIMER layout (header, due time, timer list entry, DPC, period), of which
// only the header is used: the timer engine (timers.h) keeps due time and period on the host.
struct X_KTIMER {
  X_DISPATCHER_HEADER header;  // 0x00
  be<uint64_t> due_time;       // 0x10
  X_LIST_ENTRY timer_list;     // 0x18
  be<uint32_t> dpc;            // 0x20
  be<int32_t> period;          // 0x24
};
static_assert(sizeof(X_KTIMER) == 0x28);
static_assert(offsetof(X_KTIMER, due_time) == 0x10);
static_assert(offsetof(X_KTIMER, dpc) == 0x20);
static_assert(offsetof(X_KTIMER, period) == 0x24);

// Deferred procedure call object, from Xenia's XDPC (xboxkrnl_threading.cc): seven u32. KeInitializeDpc packs the
// object type 19 (DpcObject) in the top byte of the first word, importance and number below it.
struct X_KDPC {
  be<uint32_t> type_word;  // 0x00: type << 24 | importance << 16 | number << 8
  be<uint32_t> flink;      // 0x04
  be<uint32_t> blink;      // 0x08
  be<uint32_t> routine;    // 0x0C
  be<uint32_t> context;    // 0x10
  be<uint32_t> arg1;       // 0x14
  be<uint32_t> arg2;       // 0x18
};
static_assert(sizeof(X_KDPC) == 0x1C);
static_assert(offsetof(X_KDPC, routine) == 0x0C);
static_assert(offsetof(X_KDPC, context) == 0x10);
static_assert(offsetof(X_KDPC, arg1) == 0x14);
static_assert(offsetof(X_KDPC, arg2) == 0x18);

// RtlTimeToTimeFields / RtlTimeFieldsToTime calendar fields, Xenia's X_TIME_FIELDS (xboxkrnl_rtl.cc, 16 bytes):
// eight be<int16_t>, weekday last (0 = Sunday).
struct X_TIME_FIELDS {
  be<int16_t> year;
  be<int16_t> month;  // 1..12
  be<int16_t> day;    // 1..31
  be<int16_t> hour;
  be<int16_t> minute;
  be<int16_t> second;
  be<int16_t> milliseconds;
  be<int16_t> weekday;
};
static_assert(sizeof(X_TIME_FIELDS) == 16);
static_assert(offsetof(X_TIME_FIELDS, milliseconds) == 0xC);
static_assert(offsetof(X_TIME_FIELDS, weekday) == 0xE);

// KeTimeStampBundle (kernel variable, 24 bytes as Xenia's xboxkrnl_module.cc allocates it): u64 at +0, u64 at +8,
// u32 tick count in milliseconds at +16 (the only field Xenia updates, every 1 ms), u32 padding. +0 and +8 are the
// NT KSYSTEM_TIME-style interrupt time (100 ns since boot) and system time (FILETIME); we keep them current too.
struct X_TIME_STAMP_BUNDLE {
  be<uint64_t> interrupt_time;  // 0x00
  be<uint64_t> system_time;     // 0x08
  be<uint32_t> tick_count;      // 0x10
  be<uint32_t> padding;         // 0x14
};
static_assert(sizeof(X_TIME_STAMP_BUNDLE) == 24);
static_assert(offsetof(X_TIME_STAMP_BUNDLE, system_time) == 8);
static_assert(offsetof(X_TIME_STAMP_BUNDLE, tick_count) == 0x10);

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

// RtlRaiseException's record. Verified against Xenia src/xenia/xbox.h (X_EXCEPTION_RECORD, static_assert_size 0x50).
struct X_EXCEPTION_RECORD {
  be<uint32_t> code;
  be<uint32_t> exception_flags;
  be<uint32_t> exception_record;
  be<uint32_t> exception_address;
  be<uint32_t> number_parameters;
  be<uint32_t> exception_information[15];
};
static_assert(sizeof(X_EXCEPTION_RECORD) == 0x50);
static_assert(offsetof(X_EXCEPTION_RECORD, number_parameters) == 0x10);
static_assert(offsetof(X_EXCEPTION_RECORD, exception_information) == 0x14);

// The Xbox 360 (PowerPC) CONTEXT that RtlCaptureContext fills. Xenia (master and canary) has no X_CONTEXT, so the
// layout follows the public Xbox 360 kernel type definitions Xenia cites elsewhere (xkelib keTypes.h): control
// section (flags, msr, iar, lr, ctr), 32 GPRs, cr/xer, fpscr + 32 FPRs, then the VMX section; 0xA40 bytes.
// ContextFlags bits: CONTEXT_CONTROL 0x1, CONTEXT_FLOATING_POINT 0x2, CONTEXT_INTEGER 0x4, CONTEXT_VECTOR 0x10.
struct X_CONTEXT {
  be<uint32_t> context_flags;      // 0x000
  be<uint32_t> msr;                // 0x004
  be<uint32_t> iar;                // 0x008
  be<uint32_t> lr;                 // 0x00C
  be<uint64_t> ctr;                // 0x010
  be<uint64_t> gpr[32];            // 0x018
  be<uint32_t> cr;                 // 0x118
  be<uint32_t> xer;                // 0x11C
  be<double> fpscr;                // 0x120
  be<double> fpr[32];              // 0x128
  be<uint32_t> user_mode_control;  // 0x228
  be<uint32_t> reserved;           // 0x22C
  be<uint32_t> vscr[4];            // 0x230
  be<uint32_t> vr[128][4];         // 0x240
};
static_assert(sizeof(X_CONTEXT) == 0xA40);
static_assert(offsetof(X_CONTEXT, lr) == 0xC);
static_assert(offsetof(X_CONTEXT, ctr) == 0x10);
static_assert(offsetof(X_CONTEXT, gpr) == 0x18);
static_assert(offsetof(X_CONTEXT, cr) == 0x118);
static_assert(offsetof(X_CONTEXT, fpscr) == 0x120);
static_assert(offsetof(X_CONTEXT, fpr) == 0x128);
static_assert(offsetof(X_CONTEXT, user_mode_control) == 0x228);
static_assert(offsetof(X_CONTEXT, vscr) == 0x230);
static_assert(offsetof(X_CONTEXT, vr) == 0x240);

}  // namespace lr::kernel
