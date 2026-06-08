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

#pragma once

#include "modulation/params/param.h"
#include <cstdint>

class ModControllable;

/// Consumer-agnostic "recently touched params" working set for MIDI feedback (feature A3).
///
/// It bounds the expensive feedback paths — the per-tick automation mirror and the one-shot broadcasts at
/// clip launch / playback start — to the params the user is actively working with. It is shared identically
/// by every feedback consumer (MIDI follow, learned knobs, and Default CC Input). Crucially it lives ABOVE
/// any one consumer: follow and Default CC Input resolve via the default CC->param map and have no MIDIKnob,
/// so a per-control flag could not constrain them. The working set is the one thing they all consult.
///
/// A param is identified by (mod-controllable, kind, id) — exactly the identity each consumer already holds
/// when it is about to send feedback. Membership is a pure value comparison and NEVER dereferences the
/// ModControllable pointer, so a stale pointer left behind by a deleted instrument is harmless (at worst one
/// spurious match, never a crash). The set is runtime-only and is cleared when the song is torn down.
class MidiFeedbackWorkingSet {
public:
	/// How many most-recently-touched params to remember — roughly one full controller surface.
	static constexpr int32_t kCapacity = 16;

	/// Record that the user just touched a param: a Deluge-side edit (gold knob / menu / automation pad) or a
	/// controller-side move. NOT automation playback — an automated value change is not a user touch and must
	/// not pollute the set it is used to filter. Moves the param to the front; de-duplicates; evicts the
	/// oldest once full. A null mod-controllable is ignored.
	void recordTouch(ModControllable* modControllable, deluge::modulation::params::Kind kind, int32_t paramId);

	/// Whether this param is currently in the recently-touched set.
	bool isTouched(ModControllable* modControllable, deluge::modulation::params::Kind kind, int32_t paramId) const;

	/// Forget everything — the stored ModControllable pointers become meaningless once their song is gone.
	void clear() { count = 0; }

private:
	struct Entry {
		ModControllable* modControllable;
		deluge::modulation::params::Kind kind;
		int32_t paramId;
	};
	// Most-recently-touched first; the valid range is [0, count).
	Entry entries[kCapacity];
	int32_t count = 0;
};

extern MidiFeedbackWorkingSet midiFeedbackWorkingSet;
