/*
 * Copyright © 2024 Synthstrom Audible Limited
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

#include "io/midi/midi_feedback_working_set.h"

using deluge::modulation::params::Kind;

MidiFeedbackWorkingSet midiFeedbackWorkingSet{};

void MidiFeedbackWorkingSet::recordTouch(ModControllable* modControllable, Kind kind, int32_t paramId) {
	if (modControllable == nullptr) {
		return;
	}

	// Find an existing entry for this exact param (so a re-touch just moves it back to the front).
	int32_t shiftFrom = -1;
	for (int32_t i = 0; i < count; i++) {
		if (entries[i].modControllable == modControllable && entries[i].kind == kind && entries[i].paramId == paramId) {
			shiftFrom = i;
			break;
		}
	}

	if (shiftFrom < 0) {
		// Not present: grow if there's room, otherwise the oldest (at count - 1) falls off.
		if (count < kCapacity) {
			count++;
		}
		shiftFrom = count - 1;
	}

	// Open up slot 0 by shifting everything above the (re)used slot down one.
	for (int32_t i = shiftFrom; i > 0; i--) {
		entries[i] = entries[i - 1];
	}
	entries[0] = {modControllable, kind, paramId};
}

bool MidiFeedbackWorkingSet::isTouched(ModControllable* modControllable, Kind kind, int32_t paramId) const {
	if (modControllable == nullptr) {
		return false;
	}
	for (int32_t i = 0; i < count; i++) {
		if (entries[i].modControllable == modControllable && entries[i].kind == kind && entries[i].paramId == paramId) {
			return true;
		}
	}
	return false;
}
