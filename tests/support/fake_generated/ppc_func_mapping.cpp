// SPDX-License-Identifier: MIT
#include "ppc_recomp_shared.h"

PPCFuncMapping PPCFuncMappings[] = {
	{ 0x82000100, sub_82000100 },
	{ 0x82000140, __imp__FakeImplemented },
	{ 0x82000144, __imp__FakeMissing },
	{ 0x82000148, __imp__FakeStub },
	{ 0x8200014C, __imp__FakeCallback },
	{ 0x82000150, __imp__ExCreateThread },
	{ 0x82000154, __imp__NtWaitForSingleObjectEx },
	{ 0x82000160, sub_82000160 },
	{ 0x82000170, sub_82000170 },
	{ 0x82000180, sub_82000180 },
	{ 0x820001C0, sub_820001C0 },
	{ 0x820001E0, sub_820001E0 },
	{ 0x82000200, sub_82000200 },
	{ 0x82000220, sub_82000220 },
	{ 0x82000280, sub_82000280 },
	{ 0x820002A0, sub_820002A0 },
	{ 0x820002C0, sub_820002C0 },
	{ 0x820002E0, sub_820002E0 },
	{ 0, nullptr }
};
