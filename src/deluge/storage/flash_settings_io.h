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

#pragma once

#include "definitions_cxx.hpp"
#include <cstdint>
#include <span>

namespace FlashStorage {

/// Read the persisted settings blob from flash into `buffer`. A single continuous read is fine — only
/// writes are page-limited on SPI NOR flash.
void loadSettingsBuffer(std::span<uint8_t> buffer);

/// Erase the settings sector and persist `buffer` back to flash. The write is split into
/// kFlashSettingsPageSize-sized Program commands because a single SPI Page Program cannot cross a flash
/// page boundary (see definitions_cxx.hpp). `buffer.size()` must not exceed kFlashSettingsRegionSize.
void persistSettingsBuffer(std::span<const uint8_t> buffer);

} // namespace FlashStorage
