// SPDX-License-Identifier: GPL-3.0-or-later
#include "ppc_recomp_shared.h"

PPCFuncMapping PPCFuncMappings[] = {
	{ 0x82000100, sub_82000100 },
	{ 0x82000140, __imp__FakeImplemented },
	{ 0x82000144, __imp__FakeMissing },
	{ 0x82000180, sub_82000180 },
	{ 0, nullptr }
};
