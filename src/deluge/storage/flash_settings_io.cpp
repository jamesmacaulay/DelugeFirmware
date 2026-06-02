/*
 * Copyright (c) 2026 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#include "storage/flash_settings_io.h"
#include "RZA1/cpu_specific.h" // SPIBSC_CH
#include <algorithm>

extern "C" {
#include "RZA1/spibsc/r_spibsc_flash_api.h"
#include "RZA1/spibsc/spibsc.h"
}

namespace FlashStorage {

void loadSettingsBuffer(std::span<uint8_t> buffer) {
	R_SFLASH_ByteRead(kFlashSettingsBaseAddress, buffer.data(), static_cast<int32_t>(buffer.size()), SPIBSC_CH,
	                  SPIBSC_CMNCR_BSZ_SINGLE, SPIBSC_1BIT, SPIBSC_OUTPUT_ADDR_24);
}

void persistSettingsBuffer(std::span<const uint8_t> buffer) {
	R_SFLASH_EraseSector(kFlashSettingsBaseAddress, SPIBSC_CH, SPIBSC_CMNCR_BSZ_SINGLE, 1, SPIBSC_OUTPUT_ADDR_24);
	// A single SPI Page Program cannot cross a flash page boundary, so write one page at a time.
	for (size_t offset = 0; offset < buffer.size(); offset += kFlashSettingsPageSize) {
		const size_t chunk = std::min(kFlashSettingsPageSize, buffer.size() - offset);
		R_SFLASH_ByteProgram(kFlashSettingsBaseAddress + offset, const_cast<uint8_t*>(buffer.data() + offset),
		                     static_cast<int32_t>(chunk), SPIBSC_CH, SPIBSC_CMNCR_BSZ_SINGLE, SPIBSC_1BIT,
		                     SPIBSC_OUTPUT_ADDR_24);
	}
}

} // namespace FlashStorage
