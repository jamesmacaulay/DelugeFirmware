// Regression tests for the flash settings persist/load primitive.
//
// The bug: persistSettingsBuffer originally issued a single 256-byte SPI Page Program, so any settings
// blob larger than one flash page lost everything past the first page on the next read. These tests
// round-trip a buffer that crosses the page boundary through an in-memory flash model (see
// mocks/spibsc_flash_stub.cpp) and assert nothing is dropped.
//
// They are deliberately tied to the shared kFlashSettings* constants rather than hard-coded sizes, so
// as the settings blob grows and kFlashSettingsBufferSize is raised, the coverage tracks it automatically.
#include "CppUTest/TestHarness.h"
#include "definitions_cxx.hpp"
#include "storage/flash_settings_io.h"
#include <array>
#include <cstdint>

// Accessors into the in-memory flash model (mocks/spibsc_flash_stub.cpp).
namespace test_flash {
void reset();
int32_t maxProgramSize();
size_t programCallCount();
} // namespace test_flash

namespace {
// Deterministic, position-dependent pattern so a dropped/duplicated byte is caught.
uint8_t pattern(size_t i) {
	return static_cast<uint8_t>(i * 31 + 7);
}
} // namespace

TEST_GROUP(FlashSettingsIO){void setup() override{test_flash::reset();
}
}
;

// The core regression: a full-size buffer (two pages) must survive a write/read round-trip intact.
TEST(FlashSettingsIO, roundTripFullBufferAcrossPages) {
	std::array<uint8_t, kFlashSettingsBufferSize> written;
	for (size_t i = 0; i < written.size(); ++i) {
		written[i] = pattern(i);
	}
	FlashStorage::persistSettingsBuffer(written);

	std::array<uint8_t, kFlashSettingsBufferSize> readBack;
	readBack.fill(0);
	FlashStorage::loadSettingsBuffer(readBack);

	MEMCMP_EQUAL(written.data(), readBack.data(), written.size());
}

// The minimal demonstrator: a buffer just one byte past a page boundary.
TEST(FlashSettingsIO, oneBytePastPageBoundarySurvives) {
	constexpr size_t kLen = kFlashSettingsPageSize + 1;
	std::array<uint8_t, kLen> written;
	for (size_t i = 0; i < kLen; ++i) {
		written[i] = pattern(i);
	}
	FlashStorage::persistSettingsBuffer(written);

	std::array<uint8_t, kLen> readBack;
	readBack.fill(0);
	FlashStorage::loadSettingsBuffer(readBack);

	BYTES_EQUAL(written[kFlashSettingsPageSize], readBack[kFlashSettingsPageSize]);
	MEMCMP_EQUAL(written.data(), readBack.data(), kLen);
}

// Pins the page-split contract: every Program stays within one page and the whole buffer is covered.
TEST(FlashSettingsIO, programIsSplitIntoPageSizedWrites) {
	std::array<uint8_t, kFlashSettingsBufferSize> written;
	written.fill(0xA5);
	FlashStorage::persistSettingsBuffer(written);

	CHECK_TRUE_TEXT(test_flash::maxProgramSize() <= static_cast<int32_t>(kFlashSettingsPageSize),
	                "no single Program command may exceed one flash page");
	CHECK_EQUAL_TEXT(kFlashSettingsBufferSize / kFlashSettingsPageSize, test_flash::programCallCount(),
	                 "the whole buffer must be programmed, one command per page");
}
