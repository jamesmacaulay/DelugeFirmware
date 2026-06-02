// In-memory model of the SPI NOR flash settings sector, used by flash_settings_io_tests.
//
// It deliberately models the two hardware constraints that the production code must respect:
//   1. A Page Program command can only write within a single page (it must not cross a page boundary),
//      so an oversized/misaligned program is a hard error here.
//   2. Programming can only clear bits (1 -> 0); it cannot set them. So a region must be erased
//      (all 0xFF) before it can be written, otherwise stale bits would remain.
#include "CppUTest/TestHarness.h"
#include "definitions_cxx.hpp"
#include <array>
#include <cstdint>

extern "C" {
#include "RZA1/spibsc/r_spibsc_flash_api.h"
}

namespace {
std::array<uint8_t, kFlashSettingsRegionSize> g_flash;
int32_t g_maxProgramSize = 0;
size_t g_programCallCount = 0;
} // namespace

namespace test_flash {
/// Reset the simulated sector to the erased state (all 0xFF) and clear the program counters.
void reset() {
	g_flash.fill(0xFF);
	g_maxProgramSize = 0;
	g_programCallCount = 0;
}
int32_t maxProgramSize() {
	return g_maxProgramSize;
}
size_t programCallCount() {
	return g_programCallCount;
}
} // namespace test_flash

extern "C" {

int32_t R_SFLASH_EraseSector(uint32_t addr, uint32_t /*ch_no*/, uint32_t /*dual*/, uint8_t /*data_width*/,
                             uint8_t /*addr_mode*/) {
	CHECK_EQUAL_TEXT(kFlashSettingsBaseAddress, addr, "erase address must be the settings sector base");
	g_flash.fill(0xFF);
	return 0;
}

int32_t R_SFLASH_ByteProgram(uint32_t addr, uint8_t* buf, int32_t size, uint32_t /*ch_no*/, uint32_t /*dual*/,
                             uint8_t /*data_width*/, uint8_t /*addr_mode*/) {
	const size_t offset = addr - kFlashSettingsBaseAddress;
	CHECK_TRUE_TEXT(size >= 0 && static_cast<size_t>(size) <= kFlashSettingsPageSize,
	                "Page Program must not exceed one flash page");
	CHECK_TRUE_TEXT((offset % kFlashSettingsPageSize) + static_cast<size_t>(size) <= kFlashSettingsPageSize,
	                "Page Program must not cross a flash page boundary");
	CHECK_TRUE_TEXT(offset + static_cast<size_t>(size) <= g_flash.size(), "Page Program out of region");

	g_maxProgramSize = (size > g_maxProgramSize) ? size : g_maxProgramSize;
	++g_programCallCount;

	for (int32_t i = 0; i < size; ++i) {
		g_flash[offset + i] &= buf[i]; // NOR can only clear bits; requires a prior erase
	}
	return 0;
}

int32_t R_SFLASH_ByteRead(uint32_t addr, uint8_t* buf, int32_t size, uint32_t /*ch_no*/, uint32_t /*dual*/,
                          uint8_t /*data_width*/, uint8_t /*addr_mode*/) {
	const size_t offset = addr - kFlashSettingsBaseAddress;
	CHECK_TRUE_TEXT(offset + static_cast<size_t>(size) <= g_flash.size(), "Read out of region");
	for (int32_t i = 0; i < size; ++i) {
		buf[i] = g_flash[offset + i];
	}
	return 0;
}

} // extern "C"
