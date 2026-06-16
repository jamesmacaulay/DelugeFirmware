/*
 * Copyright © 2016-2023 Synthstrom Audible Limited
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

#include "modulation/automation/auto_param.h"
#include <cstdint>

/// A per-clip-per-param "transformation space": a piecewise-linear map from the param's value
/// range onto itself. The param's ordinary value/automation acts as the *index* into the map,
/// and the map's output is what the rest of the engine hears (applied in
/// AutoParam::getCurrentValue()).
///
/// Two rail anchors are always present, implicit and immutable: map(INT32_MIN) = INT32_MIN and
/// map(INT32_MAX) = INT32_MAX. Because the map is continuous with fixed endpoints, every output
/// value in the param's range is reachable at every moment regardless of the interior nodes
/// (intermediate value theorem). Zero interior nodes = identity = stock behaviour; an AutoParam
/// with no ParamLandscape allocated behaves exactly as on stock firmware.
///
/// Interior node values are full AutoParams: a constant when un-automated, or a time-automated
/// lane evaluated at the current playback position before interpolating across positions. Node
/// lanes never have landscapes of their own.
class ParamLandscape {
public:
	/// Engine-level cap on interior nodes. The serialized format carries an explicit count, so
	/// this can be raised without a file-format change.
	static constexpr int32_t kMaxInteriorNodes = 4;

	struct Node {
		/// Where along the param's value range this node sits (exclusive of the int32 rails).
		/// Strictly increasing across ParamLandscape::nodes.
		int32_t position;
		/// Output value at this position. May be automated (then it's evaluated at the playback
		/// position first). Its `landscape` member must remain nullptr.
		AutoParam value;
	};

	ParamLandscape() : numNodes(0) {}

	/// Piecewise-linear map at the current moment (node lanes contribute their currentValue).
	/// Allocation-free; safe on the audio/tick path. Returns the smoothed slide output while
	/// the interpolator is active; transformRaw() is the unsmoothed evaluation.
	int32_t transform(int32_t index);
	int32_t transformRaw(int32_t index);

	/// Same map as transform(), but with one lane's value overridden — used to reconstruct the
	/// previous output when translating a lane-stacked notification into output space.
	int32_t transformWithLaneValue(int32_t index, int32_t laneIndex, int32_t laneValue);

	/// Same map, but with automated node lanes evaluated at a specific timeline position
	/// instead of their live current values. For rendering and flattening, not the tick path.
	int32_t transformAtPos(int32_t index, uint32_t pos, ModelStackWithAutoParam const* modelStack);

	/// True if any interior node's value lane contains automation nodes (and therefore needs the
	/// engine to tick it).
	bool hasAnyAutomatedLanes();

	/// Swap two interior nodes' contents (positions and lanes) — used when a node is dragged
	/// past a neighbour so the array stays sorted while the lanes keep their identities.
	void swapNodes(int32_t a, int32_t b);

	/// Deep-copy from another landscape onto this (freshly constructed) one. With
	/// copyAutomation false, node positions and constant values are kept but lane automation is
	/// dropped — matching AutoParam::cloneFrom() semantics.
	void cloneFrom(ParamLandscape* other, bool copyAutomation);

	int32_t numNodes;
	Node nodes[kMaxInteriorNodes];

	// --- Slide morph (phase-aware morphing between adjacent saved positions) ---
	// When both lanes of an interior segment are automated with EQUAL node counts, the morph
	// pairs their nodes (cyclically rotated for best phase alignment) and slides each pair's
	// position and value with the morph fraction — out-of-phase patterns morph through phase,
	// never through a flat blend. Unequal counts (and rail segments) keep the pointwise blend.

	/// Playback context cached each tick by the owning param's processCurrentPos, so slide
	/// evaluation stays a pure function without threading time through getCurrentValue().
	int32_t lastPlayPos = 0;
	int32_t loopLength = 0;

	/// Sample-smoothing interpolator for sliding segments: the audible output ramps toward
	/// each tick's exact evaluation instead of stepping at tick rate (which zipper-ed on dense
	/// lanes), and discontinuous reshapes (live lane edits change pairing globally) become
	/// ~1-tick glides instead of clicks. Active only while playback ticks a sliding segment.
	int32_t slideOutputValue = 0;
	int32_t slideIncrementPerHalfTick = 0;
	bool slideOutputActive = false;

	/// Quantised recall: >= 0 arms that slot to be recalled (index cleared and parked on it)
	/// at the next loop boundary. Transient — never serialised. -1 = nothing armed.
	int32_t pendingRecallSlot = -1;

	/// Does the segment between interior nodes leftIndex and leftIndex+1 slide-morph?
	bool segmentSlides(int32_t leftIndex);
	/// Is the given index inside a sliding segment? (Scheduling: such a param must be ticked
	/// every tick, since its output moves with the playhead even between lane events.)
	bool slidePossibleForIndex(int32_t index);

	/// Evaluate the sliding segment at morph fraction morphFrac (Q31, 0..2^31 inclusive) and
	/// timeline position t. Pure; O(lane nodes); no allocation.
	int32_t evaluateSlideSegment(int32_t leftIndex, uint32_t morphFrac, int32_t t);

	/// Materialise the sliding segment's morphed breakpoints (sorted, deduplicated) into dest
	/// — the EXACT node list of the output at this morph fraction, for lossless capture.
	Error buildSlideNodeList(int32_t leftIndex, uint32_t morphFrac, ParamNodeVector* dest);

private:
	/// Cyclic rotation pairing lane A's node i with lane B's node (i+rotation)%n, chosen to
	/// minimise total (short-way) time displacement. Cached per segment; the cheap fingerprint
	/// detects lane edits and triggers an O(n^2) re-pick.
	int32_t getSlideRotation(int32_t leftIndex);
	uint32_t slideFingerprint[kMaxInteriorNodes - 1] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
	int32_t slideRotation[kMaxInteriorNodes - 1] = {0, 0, 0};
};
