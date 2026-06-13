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

#include "modulation/automation/param_landscape.h"
#include "model/settings/runtime_feature_settings.h"
#include "modulation/params/param_node.h"
#include <algorithm>

namespace {

// Linear interpolation between (x0,y0) and (x1,y1), evaluated at x, where all coordinates span
// the full int32 range. dx is kept strictly below the segment span so the Q31 fraction stays
// below 2^31, which keeps frac*dy within int64.
int32_t lerpSegment(int32_t x, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
	// x1 checked first so a node parked exactly on the bottom rail (x0 == x1 == INT32_MIN)
	// OVERRIDES the implicit rail anchor: rails are defaults, not pinned. (A node on the top
	// rail already overrides structurally — it terminates the loop before the rail segment.)
	if (x >= x1) {
		return y1;
	}
	if (x <= x0) {
		return y0;
	}
	uint32_t span = (uint32_t)((int64_t)x1 - (int64_t)x0);
	uint32_t dx = (uint32_t)((int64_t)x - (int64_t)x0);
	uint64_t frac = ((uint64_t)dx << 31) / span; // Q31, < 2^31 because dx < span
	int64_t dy = (int64_t)y1 - (int64_t)y0;
	return (int32_t)((int64_t)y0 + (((int64_t)frac * dy) >> 31));
}

} // namespace

int32_t ParamLandscape::transformWithLaneValue(int32_t index, int32_t laneIndex, int32_t laneValue) {
	int32_t leftPos = INT32_MIN;
	int32_t leftValue = INT32_MIN;

	for (int32_t i = 0; i < numNodes; i++) {
		int32_t rightPos = nodes[i].position;
		int32_t rightValue = (i == laneIndex) ? laneValue : nodes[i].value.getCurrentValue();
		if (index <= rightPos) {
			return lerpSegment(index, leftPos, leftValue, rightPos, rightValue);
		}
		leftPos = rightPos;
		leftValue = rightValue;
	}

	return lerpSegment(index, leftPos, leftValue, INT32_MAX, INT32_MAX);
}

int32_t ParamLandscape::transformAtPos(int32_t index, uint32_t pos, ModelStackWithAutoParam const* modelStack) {
	int32_t leftPos = INT32_MIN;
	int32_t leftValue = INT32_MIN;

	for (int32_t i = 0; i < numNodes; i++) {
		AutoParam& lane = nodes[i].value;
		int32_t rightPos = nodes[i].position;
		if (index <= rightPos) {
			if (i > 0 && segmentSlides(i - 1)) {
				uint32_t span = (uint32_t)((int64_t)rightPos - (int64_t)leftPos);
				uint32_t morphFrac = (uint32_t)((((uint64_t)(uint32_t)((int64_t)index - (int64_t)leftPos)) << 31) / span);
				return evaluateSlideSegment(i - 1, morphFrac, (int32_t)pos);
			}
			int32_t rightValue = lane.isAutomated() ? lane.getValueAtPos(pos, modelStack) : lane.getCurrentIndexValue();
			return lerpSegment(index, leftPos, leftValue, rightPos, rightValue);
		}
		leftPos = rightPos;
		leftValue = lane.isAutomated() ? lane.getValueAtPos(pos, modelStack) : lane.getCurrentIndexValue();
	}

	return lerpSegment(index, leftPos, leftValue, INT32_MAX, INT32_MAX);
}

int32_t ParamLandscape::transform(int32_t index) {
	if (slideOutputActive) {
		return slideOutputValue;
	}
	return transformRaw(index);
}

int32_t ParamLandscape::transformRaw(int32_t index) {
	int32_t leftPos = INT32_MIN;
	int32_t leftValue = INT32_MIN;

	for (int32_t i = 0; i < numNodes; i++) {
		int32_t rightPos = nodes[i].position;
		if (index <= rightPos) {
			// Interior segments with both lanes automated at equal node counts slide-morph
			// (phase-aware) instead of pointwise-blending. The play position comes from the
			// per-tick cache, keeping this a pure function of the index.
			if (i > 0 && segmentSlides(i - 1)) {
				uint32_t span = (uint32_t)((int64_t)rightPos - (int64_t)leftPos);
				uint32_t morphFrac = (uint32_t)((((uint64_t)(uint32_t)((int64_t)index - (int64_t)leftPos)) << 31) / span);
				return evaluateSlideSegment(i - 1, morphFrac, lastPlayPos);
			}
			return lerpSegment(index, leftPos, leftValue, rightPos, nodes[i].value.getCurrentValue());
		}
		leftPos = rightPos;
		leftValue = nodes[i].value.getCurrentValue();
	}

	return lerpSegment(index, leftPos, leftValue, INT32_MAX, INT32_MAX);
}

void ParamLandscape::swapNodes(int32_t a, int32_t b) {
	Node& nodeA = nodes[a];
	Node& nodeB = nodes[b];

	int32_t tempPosition = nodeA.position;
	nodeA.position = nodeB.position;
	nodeB.position = tempPosition;

	nodeA.value.nodes.swapStateWith(&nodeB.value.nodes);

	int32_t tempValue = nodeA.value.currentValue;
	nodeA.value.currentValue = nodeB.value.currentValue;
	nodeB.value.currentValue = tempValue;

	int32_t tempIncrement = nodeA.value.valueIncrementPerHalfTick;
	nodeA.value.valueIncrementPerHalfTick = nodeB.value.valueIncrementPerHalfTick;
	nodeB.value.valueIncrementPerHalfTick = tempIncrement;

	uint32_t tempOverriding = nodeA.value.renewedOverridingAtTime;
	nodeA.value.renewedOverridingAtTime = nodeB.value.renewedOverridingAtTime;
	nodeB.value.renewedOverridingAtTime = tempOverriding;
}

void ParamLandscape::cloneFrom(ParamLandscape* other, bool copyAutomation) {
	numNodes = other->numNodes;
	for (int32_t i = 0; i < numNodes; i++) {
		nodes[i].position = other->nodes[i].position;
		nodes[i].value.cloneFrom(&other->nodes[i].value, copyAutomation);
	}
}

bool ParamLandscape::hasAnyAutomatedLanes() {
	for (int32_t i = 0; i < numNodes; i++) {
		if (nodes[i].value.isAutomated()) {
			return true;
		}
	}
	return false;
}


// --- Slide morph ---

namespace {

// Shortest cyclic displacement from a to b on a loop of length L.
int32_t shortWayDelta(int32_t a, int32_t b, int32_t loopLength) {
	int32_t delta = b - a;
	if (delta > (loopLength >> 1)) {
		delta -= loopLength;
	}
	else if (delta < -(loopLength >> 1)) {
		delta += loopLength;
	}
	return delta;
}

// The point a lane contributes to pair k of nPairs. When the lane has fewer nodes than
// nPairs, intermediate pairs get VIRTUAL points interpolated on the lane's existing line
// (positionally between its real nodes, value on the line) — shape-neutral, so the morph
// endpoints still reproduce each lane exactly. With laneNodes == nPairs every point is real.
struct SlidePoint {
	int32_t pos;
	int32_t value;
	bool interpolated;
};

SlidePoint lanePointForPair(AutoParam& lane, int32_t pairIndex, int32_t nPairs, int32_t loopLength) {
	int32_t numLaneNodes = lane.nodes.getNumElements();
	int32_t scaled = pairIndex * numLaneNodes; // parameter position * nPairs
	int32_t j = scaled / nPairs;
	int32_t remainder = scaled - (j * nPairs); // fraction numerator, denominator nPairs

	ParamNode* nodeJ = lane.nodes.getElement(j);
	if (remainder == 0) {
		return SlidePoint{nodeJ->pos, nodeJ->value, nodeJ->interpolated};
	}

	int32_t jNext = j + 1;
	if (jNext >= numLaneNodes) {
		jNext = 0;
	}
	ParamNode* nodeNext = lane.nodes.getElement(jNext);

	int32_t segmentSpan = nodeNext->pos - nodeJ->pos;
	if (segmentSpan <= 0) {
		segmentSpan += loopLength; // Wrapping segment through the loop point.
	}
	int32_t pos = nodeJ->pos + (int32_t)((int64_t)segmentSpan * remainder / nPairs);
	if (pos >= loopLength) {
		pos -= loopLength;
	}

	// On-the-line value: mid-ramp for interpolated segments, held value through steps.
	int32_t value = nodeNext->interpolated
	                    ? nodeJ->value + (int32_t)((int64_t)(nodeNext->value - nodeJ->value) * remainder / nPairs)
	                    : nodeJ->value;

	return SlidePoint{pos, value, true};
}

uint32_t laneSlideFingerprint(AutoParam& lane) {
	uint32_t fingerprint = (uint32_t)lane.nodes.getNumElements() * 2654435761u;
	for (int32_t n = 0; n < lane.nodes.getNumElements(); n++) {
		ParamNode* node = lane.nodes.getElement(n);
		fingerprint = fingerprint * 31 + (uint32_t)node->pos + ((uint32_t)node->interpolated << 30);
	}
	return fingerprint;
}

} // namespace

bool ParamLandscape::segmentSlides(int32_t leftIndex) {
	// Community toggle: Off = pointwise crossfade everywhere (slide machinery kept but
	// dormant — scheduling, smoothing and the exact-capture paths all key off this).
	if (runtimeFeatureSettings.get(RuntimeFeatureSettingType::SlideMorph) != RuntimeFeatureStateToggle::On) {
		return false;
	}
	if (leftIndex < 0 || leftIndex + 1 >= numNodes || loopLength <= 0) {
		return false;
	}
	// Both lanes automated; counts may differ (the sparser side gets virtual on-the-line
	// partners).
	return nodes[leftIndex].value.nodes.getNumElements() && nodes[leftIndex + 1].value.nodes.getNumElements();
}

bool ParamLandscape::slidePossibleForIndex(int32_t index) {
	for (int32_t i = 0; i < numNodes; i++) {
		if (index <= nodes[i].position) {
			return segmentSlides(i - 1);
		}
	}
	return false; // Top rail segment.
}

int32_t ParamLandscape::getSlideRotation(int32_t leftIndex) {
	AutoParam& laneA = nodes[leftIndex].value;
	AutoParam& laneB = nodes[leftIndex + 1].value;

	uint32_t fingerprint = laneSlideFingerprint(laneA) * 3 + laneSlideFingerprint(laneB) + (uint32_t)loopLength * 7;
	if (fingerprint == slideFingerprint[leftIndex]) {
		return slideRotation[leftIndex];
	}

	int32_t numPairs = std::max(laneA.nodes.getNumElements(), laneB.nodes.getNumElements());
	int32_t bestRotation = 0;
	int64_t bestCost = INT64_MAX;

	// O(n^2) alignment, only on lane edits. For very dense (recorded) lanes, sample the
	// candidate rotations so the spike stays bounded; alignment precision degrades gracefully.
	int32_t rotationStep = (numPairs > 128) ? (numPairs / 128) : 1;

	for (int32_t rotation = 0; rotation < numPairs; rotation += rotationStep) {
		int64_t cost = 0;
		for (int32_t i = 0; i < numPairs; i++) {
			int32_t k = i + rotation;
			if (k >= numPairs) {
				k -= numPairs;
			}
			SlidePoint pointA = lanePointForPair(laneA, i, numPairs, loopLength);
			SlidePoint pointB = lanePointForPair(laneB, k, numPairs, loopLength);
			int32_t delta = shortWayDelta(pointA.pos, pointB.pos, loopLength);
			cost += (delta >= 0) ? delta : -delta;
		}
		if (cost < bestCost) {
			bestCost = cost;
			bestRotation = rotation;
		}
	}

	// Hysteresis: while a lane is being edited the alignment is re-derived constantly; keep
	// the previous rotation unless the new optimum is clearly better, so the pairing doesn't
	// flap (audible as the whole morph reshaping) on every edit pulse.
	int32_t previousRotation = slideRotation[leftIndex];
	if (previousRotation > 0 && previousRotation < numPairs && previousRotation != bestRotation) {
		int64_t previousCost = 0;
		for (int32_t i = 0; i < numPairs; i++) {
			int32_t k = i + previousRotation;
			if (k >= numPairs) {
				k -= numPairs;
			}
			SlidePoint pointA = lanePointForPair(laneA, i, numPairs, loopLength);
			SlidePoint pointB = lanePointForPair(laneB, k, numPairs, loopLength);
			int32_t delta = shortWayDelta(pointA.pos, pointB.pos, loopLength);
			previousCost += (delta >= 0) ? delta : -delta;
		}
		if (bestCost >= previousCost - (previousCost >> 3)) { // New must win by >12.5%.
			bestRotation = previousRotation;
		}
	}

	slideFingerprint[leftIndex] = fingerprint;
	slideRotation[leftIndex] = bestRotation;
	return bestRotation;
}

Error ParamLandscape::buildSlideNodeList(int32_t leftIndex, uint32_t morphFrac, ParamNodeVector* dest) {
	AutoParam& laneA = nodes[leftIndex].value;
	AutoParam& laneB = nodes[leftIndex + 1].value;
	int32_t numPairs = std::max(laneA.nodes.getNumElements(), laneB.nodes.getNumElements());
	int32_t rotation = getSlideRotation(leftIndex);

	Error error = dest->insertAtIndex(0, numPairs);
	if (error != Error::NONE) {
		return error;
	}

	bool pickBFlags = (morphFrac >= 0x40000000);

	for (int32_t i = 0; i < numPairs; i++) {
		SlidePoint pointA = lanePointForPair(laneA, i, numPairs, loopLength);
		int32_t k = i + rotation;
		if (k >= numPairs) {
			k -= numPairs;
		}
		SlidePoint pointB = lanePointForPair(laneB, k, numPairs, loopLength);

		int32_t delta = shortWayDelta(pointA.pos, pointB.pos, loopLength);
		int32_t q = pointA.pos + (int32_t)(((int64_t)morphFrac * delta) >> 31);
		q %= loopLength;
		if (q < 0) {
			q += loopLength;
		}

		ParamNode* node = (ParamNode*)dest->getElementAddress(i);
		node->pos = q;
		node->value = pointA.value + (int32_t)(((int64_t)morphFrac * ((int64_t)pointB.value - pointA.value)) >> 31);
		node->interpolated = pickBFlags ? pointB.interpolated : pointA.interpolated;
	}

	// Morphed breakpoints can wrap out of array order — insertion-sort by position (small n,
	// edit-time only), then drop exact-position duplicates.
	for (int32_t i = 1; i < dest->getNumElements(); i++) {
		ParamNode current = *(ParamNode*)dest->getElementAddress(i);
		int32_t j = i - 1;
		while (j >= 0 && ((ParamNode*)dest->getElementAddress(j))->pos > current.pos) {
			*(ParamNode*)dest->getElementAddress(j + 1) = *(ParamNode*)dest->getElementAddress(j);
			j--;
		}
		*(ParamNode*)dest->getElementAddress(j + 1) = current;
	}
	for (int32_t i = dest->getNumElements() - 1; i > 0; i--) {
		if (((ParamNode*)dest->getElementAddress(i))->pos == ((ParamNode*)dest->getElementAddress(i - 1))->pos) {
			dest->deleteAtIndex(i);
		}
	}

	return Error::NONE;
}

int32_t ParamLandscape::evaluateSlideSegment(int32_t leftIndex, uint32_t morphFrac, int32_t t) {
	AutoParam& laneA = nodes[leftIndex].value;
	AutoParam& laneB = nodes[leftIndex + 1].value;
	int32_t numPairs = std::max(laneA.nodes.getNumElements(), laneB.nodes.getNumElements());
	int32_t rotation = getSlideRotation(leftIndex);

	t %= loopLength;
	if (t < 0) {
		t += loopLength;
	}

	// One pass over the pairs, tracking the morphed breakpoint at-or-before t and the one
	// after it (each with cyclic fallbacks), so nothing is materialised — Wstack-safe for
	// lanes of any size.
	bool haveBefore = false, haveAfter = false;
	int32_t beforeQ = 0, afterQ = 0, maxQ = 0, minQ = 0;
	int32_t beforeW = 0, afterW = 0, maxW = 0, minW = 0;
	bool afterInterp = false, minInterp = false;
	bool haveAny = false;

	bool pickBFlags = (morphFrac >= 0x40000000); // Past halfway, step/ramp character follows B.

	for (int32_t i = 0; i < numPairs; i++) {
		SlidePoint pointA = lanePointForPair(laneA, i, numPairs, loopLength);
		int32_t k = i + rotation;
		if (k >= numPairs) {
			k -= numPairs;
		}
		SlidePoint pointB = lanePointForPair(laneB, k, numPairs, loopLength);

		int32_t delta = shortWayDelta(pointA.pos, pointB.pos, loopLength);
		int32_t q = pointA.pos + (int32_t)(((int64_t)morphFrac * delta) >> 31);
		q %= loopLength;
		if (q < 0) {
			q += loopLength;
		}
		int32_t w = pointA.value + (int32_t)(((int64_t)morphFrac * ((int64_t)pointB.value - pointA.value)) >> 31);
		bool interp = pickBFlags ? pointB.interpolated : pointA.interpolated;

		if (!haveAny || q > maxQ) {
			maxQ = q;
			maxW = w;
		}
		if (!haveAny || q < minQ) {
			minQ = q;
			minW = w;
			minInterp = interp;
		}
		haveAny = true;
		if (q <= t && (!haveBefore || q > beforeQ)) {
			haveBefore = true;
			beforeQ = q;
			beforeW = w;
		}
		if (q > t && (!haveAfter || q < afterQ)) {
			haveAfter = true;
			afterQ = q;
			afterW = w;
			afterInterp = interp;
		}
	}

	// Resolve cyclic fallbacks: no breakpoint at-or-before t means the last one wraps from the
	// loop end; none after t means the first one wraps past it.
	int32_t leftQ = haveBefore ? beforeQ : (maxQ - loopLength);
	int32_t leftW = haveBefore ? beforeW : maxW;
	int32_t rightQ = haveAfter ? afterQ : (minQ + loopLength);
	int32_t rightW = haveAfter ? afterW : minW;
	bool rightInterp = haveAfter ? afterInterp : minInterp;

	if (!rightInterp) {
		return leftW; // Step: hold until the next breakpoint.
	}

	int32_t span = rightQ - leftQ;
	if (span <= 0) {
		return rightW;
	}
	return leftW + (int32_t)((int64_t)(rightW - leftW) * (t - leftQ) / span);
}
