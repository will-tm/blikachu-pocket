/* SPDX-License-Identifier: GPL-2.0-only */
#include "mem/rtc.h"
#include "savestate.h"
#include <cassert>
#include <cstdio>

int main()
{
	using gambatte::Rtc;
	using gambatte::SaveState;
	constexpr uint64_t period = 0x1ffULL * 86400;
	Rtc rtc;
	SaveState state{};
	state.rtc.dataDh = 0x40; // halted: deterministic clock value
	state.rtc.haltTime = 100;
	state.rtc.baseTime = 160;
	rtc.loadState(state);
	rtc.latch(1); // the saved base is ahead of the host clock
	assert(rtc.getBaseTime() == 100);
	rtc.saveState(state);
	assert(state.rtc.dataS == 0 && state.rtc.dataDl == 0);

	state = {};
	state.rtc.dataDh = 0x40;
	state.rtc.haltTime = 3 * period + 3661;
	rtc.loadState(state);
	rtc.latch(1);
	rtc.saveState(state);
	assert(rtc.getBaseTime() == 3 * period);
	assert(state.rtc.dataH == 1 && state.rtc.dataM == 1 && state.rtc.dataS == 1);
	assert(state.rtc.dataDh & 0x80);

	state = {};
	state.rtc.dataDh = 0x40;
	state.rtc.haltTime = 10;
	rtc.loadState(state);
	rtc.setBaseTime(UINT64_MAX - 49); // negative base represented modulo 2^64
	rtc.latch(1);
	rtc.saveState(state);
	assert(state.rtc.dataM == 1 && state.rtc.dataS == 0);
	puts("Gambatte RTC: backward clock, multi-period wrap and modular base passed");
}
