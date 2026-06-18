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

#include "model/note/note_landscape.h"
#include "memory/general_memory_allocator.h"
#include "model/clip/instrument_clip.h"
#include "model/model_stack.h"
#include "model/note/note.h"
#include "model/note/note_row.h"
#include "modulation/automation/auto_param.h"
#include "modulation/params/param_node.h"
#include "modulation/params/param_node_vector.h"
#include "storage/storage_manager.h"
#include <algorithm>
#include <cstring>
#include <new>

namespace {

inline int32_t absI(int32_t x) {
	return (x < 0) ? -x : x;
}

// 32-bit integer hash (Murmur3 finaliser variant) — well-scattered so the per-slot dither
// thresholds spread out (blue-noise-ish reveal order, not whole-row-at-once).
inline uint32_t hashU32(uint32_t x) {
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

// Dither value in [0, 0xFFFF] for a MATCHED pair {a, b}, at quantized crossfade step `step`. Folding
// the step into the hash makes EACH step an independent draw (vs a single fixed threshold) — so as the
// knob sweeps A→B the choice is re-rolled at probability t each step: the pair shimmers back and forth
// rather than flipping once and sticking, while the overall trend still favours b as t grows.
inline int32_t ditherPair(int32_t ya, int32_t pa, int32_t yb, int32_t pb, int32_t step) {
	uint32_t key = (uint32_t)ya * 2654435761U;
	key ^= (uint32_t)pa * 40503U + 0x9e3779b9U;
	key = hashU32(key) + (uint32_t)yb * 2246822519U;
	key ^= (uint32_t)pb * 3266489917U;
	key += (uint32_t)step * 0x9e3779b9U; // independent re-roll per quantized step
	return (int32_t)(hashU32(key) & 0xFFFF);
}

// Dither value for a ONE-SIDED note (the other categorical outcome is "nothing"), at step `step`.
// side distinguishes a left-only from a right-only note so they don't share a threshold by accident;
// the step makes its presence an independent draw each step (it shimmers in/out as the knob sweeps).
inline int32_t ditherOneSided(int32_t y, int32_t pos, int32_t side, int32_t step) {
	uint32_t key = (uint32_t)y * 2654435761U;
	key ^= (uint32_t)pos * 40503U;
	key ^= (uint32_t)side * 0x85ebca6bU + 0x9e3779b9U;
	key += (uint32_t)step * 0x27d4eb2fU; // independent re-roll per quantized step
	return (int32_t)(hashU32(key) & 0xFFFF);
}

// Assemble the morph's target note set for a resolved bracket (L,R) at crossfade weight t / knob step
// `step`: greedy time-match, then rank-based per-category selection (count = blend fraction t,
// identity shuffled by the per-step dither with kNoteLandscapeShimmer inertia), sorted by (rowId,
// pos). Allocates *outTarget (caller frees). Shared by computeMorph (current index) and the
// per-position automation bake.
//
// HEAP ON THE PLAYBACK PATH: this (and its callers realize()/realizeAutomatedOutput()) run from
// InstrumentClip::processCurrentPos and briefly delugeAlloc/delugeDealloc the target + sort scratch.
// The morph is self-gated on the quantized knob step (and the bake on an automation signature), so
// STEADY-STATE playback allocates nothing — only an actual morph change (knob step crossing, or an
// automation edit) re-allocates, the same as ordinary note editing already does on this path. If
// that ever proves too costly, swap these for fixed static scratch buffers sized to a worst-case
// note budget (kNoteLandscapeMaxPatterns rows * max notes), trading a few KB of permanent RAM for
// zero allocation here.
void assembleMorphTarget(const LandscapeNote* L, int32_t lN, const LandscapeNote* R, int32_t rN, int32_t t,
                         int32_t knobStep, int32_t timeRadius, LandscapeNote** outTarget, int32_t* outCount) {
	int32_t cap = lN + rN;
	LandscapeNote* target = (cap > 0) ? (LandscapeNote*)delugeAlloc(sizeof(LandscapeNote) * cap) : nullptr;
	int32_t targetCount = 0;

	int16_t* leftMatch = (lN > 0) ? (int16_t*)delugeAlloc(sizeof(int16_t) * lN) : nullptr; // right idx or -1
	uint8_t* matchedR = (rN > 0) ? (uint8_t*)delugeAlloc(rN) : nullptr;
	if (matchedR) {
		for (int32_t j = 0; j < rN; j++) {
			matchedR[j] = 0;
		}
	}
	for (int32_t a = 0; a < lN; a++) {
		const LandscapeNote& na = L[a];
		int32_t bestJ = -1;
		int64_t bestCost = INT64_MAX;
		if (matchedR) {
			for (int32_t j = 0; j < rN; j++) {
				if (matchedR[j]) {
					continue;
				}
				const LandscapeNote& nb = R[j];
				int32_t dpos = absI(na.pos - nb.pos);
				if (dpos > timeRadius) {
					continue;
				}
				int32_t dy = absI((int32_t)na.y - (int32_t)nb.y);
				int64_t cost = (int64_t)dpos * 128 + dy;
				if (cost < bestCost) {
					bestCost = cost;
					bestJ = j;
				}
			}
		}
		if (leftMatch) {
			leftMatch[a] = (int16_t)bestJ;
		}
		if (bestJ >= 0 && matchedR) {
			matchedR[bestJ] = 1;
		}
	}

	auto countFor = [](int32_t weight, int32_t n) {
		return (int32_t)(((int64_t)weight * n + kNoteLandscapeIndexMax / 2) / kNoteLandscapeIndexMax);
	};
	constexpr int32_t kStableW = kNoteLandscapeShimmerScale - kNoteLandscapeShimmer;
	auto blendPair = [&](int16_t a) -> int64_t {
		int16_t b = leftMatch[a];
		int32_t stable = ditherPair(L[a].y, L[a].pos, R[b].y, R[b].pos, 0);
		int32_t jitter = ditherPair(L[a].y, L[a].pos, R[b].y, R[b].pos, knobStep);
		return (int64_t)kStableW * stable + (int64_t)kNoteLandscapeShimmer * jitter;
	};
	auto blendOne = [&](const LandscapeNote& n, int32_t side) -> int64_t {
		int32_t stable = ditherOneSided(n.y, n.pos, side, 0);
		int32_t jitter = ditherOneSided(n.y, n.pos, side, knobStep);
		return (int64_t)kStableW * stable + (int64_t)kNoteLandscapeShimmer * jitter;
	};

	// PAIRS: the nB with the smallest dither resolve to b (switched); the rest to a.
	if (target && leftMatch) {
		int32_t numPairs = 0;
		for (int32_t a = 0; a < lN; a++) {
			if (leftMatch[a] >= 0) {
				numPairs++;
			}
		}
		if (numPairs > 0) {
			int16_t* order = (int16_t*)delugeAlloc(sizeof(int16_t) * numPairs);
			if (order) {
				int32_t w = 0;
				for (int32_t a = 0; a < lN; a++) {
					if (leftMatch[a] >= 0) {
						order[w++] = (int16_t)a;
					}
				}
				std::sort(order, order + numPairs, [&](int16_t x, int16_t y) {
					int64_t ux = blendPair(x);
					int64_t uy = blendPair(y);
					return (ux != uy) ? (ux < uy) : (x < y);
				});
				int32_t nB = countFor(t, numPairs);
				for (int32_t k = 0; k < numPairs; k++) {
					int32_t a = order[k];
					target[targetCount++] = (k < nB) ? R[leftMatch[a]] : L[a];
				}
				delugeDealloc(order);
			}
		}
	}

	// LEFT-ONLY notes (no match in b): present count = (1 - t) fraction — they fade out as t → max.
	if (target && leftMatch) {
		int32_t numLeftOnly = 0;
		for (int32_t a = 0; a < lN; a++) {
			if (leftMatch[a] < 0) {
				numLeftOnly++;
			}
		}
		if (numLeftOnly > 0) {
			int16_t* order = (int16_t*)delugeAlloc(sizeof(int16_t) * numLeftOnly);
			if (order) {
				int32_t w = 0;
				for (int32_t a = 0; a < lN; a++) {
					if (leftMatch[a] < 0) {
						order[w++] = (int16_t)a;
					}
				}
				std::sort(order, order + numLeftOnly, [&](int16_t x, int16_t y) {
					int64_t ux = blendOne(L[x], 0);
					int64_t uy = blendOne(L[y], 0);
					return (ux != uy) ? (ux < uy) : (x < y);
				});
				int32_t nPresent = countFor(kNoteLandscapeIndexMax - t, numLeftOnly);
				for (int32_t k = 0; k < nPresent; k++) {
					target[targetCount++] = L[order[k]];
				}
				delugeDealloc(order);
			}
		}
	}

	// RIGHT-ONLY notes (no match in a): present count = t fraction — they fade in as t → max.
	if (target) {
		int32_t numRightOnly = 0;
		for (int32_t j = 0; j < rN; j++) {
			if (!matchedR || !matchedR[j]) {
				numRightOnly++;
			}
		}
		if (numRightOnly > 0) {
			int16_t* order = (int16_t*)delugeAlloc(sizeof(int16_t) * numRightOnly);
			if (order) {
				int32_t w = 0;
				for (int32_t j = 0; j < rN; j++) {
					if (!matchedR || !matchedR[j]) {
						order[w++] = (int16_t)j;
					}
				}
				std::sort(order, order + numRightOnly, [&](int16_t x, int16_t y) {
					int64_t ux = blendOne(R[x], 1);
					int64_t uy = blendOne(R[y], 1);
					return (ux != uy) ? (ux < uy) : (x < y);
				});
				int32_t nPresent = countFor(t, numRightOnly);
				for (int32_t k = 0; k < nPresent; k++) {
					target[targetCount++] = R[order[k]];
				}
				delugeDealloc(order);
			}
		}
	}

	if (leftMatch) {
		delugeDealloc(leftMatch);
	}
	if (matchedR) {
		delugeDealloc(matchedR);
	}
	if (target && targetCount > 1) {
		std::sort(target, target + targetCount, [](const LandscapeNote& a, const LandscapeNote& b) {
			return (a.y != b.y) ? (a.y < b.y) : (a.pos < b.pos);
		});
	}
	*outTarget = target;
	*outCount = targetCount;
}

// Evaluate an AutoParam's value at an arbitrary loop position by walking its nodes — a copy of
// AutoParam::getValueAtPos (forward play) that takes the loop length directly, so it can be called
// off the audio path without building a ModelStackWithAutoParam.
int32_t evalAutomationAt(AutoParam* param, uint32_t pos, int32_t loopLength) {
	ParamNodeVector& nodes = param->nodes;
	int32_t n = nodes.getNumElements();
	if (!n) {
		return param->currentValue;
	}
	int32_t rightI = nodes.search((int32_t)pos + 1, GREATER_OR_EQUAL);
	if (rightI >= n) {
		rightI = 0;
	}
	ParamNode* rightNode = nodes.getElement(rightI);
	int32_t leftI = rightI - 1;
	if (leftI < 0) {
		leftI += n;
	}
	ParamNode* leftNode = nodes.getElement(leftI);
	if (!rightNode->interpolated) {
		return leftNode->value;
	}
	int32_t ticksSinceLeftNode = (int32_t)pos - leftNode->pos;
	if (ticksSinceLeftNode == 0) {
		return leftNode->value;
	}
	if (ticksSinceLeftNode < 0) {
		if (loopLength == 2147483647) {
			return rightNode->value;
		}
		ticksSinceLeftNode += loopLength;
	}
	int32_t ticksBetweenNodes = rightNode->pos - leftNode->pos;
	if (ticksBetweenNodes <= 0) {
		if (loopLength == 2147483647) {
			return leftNode->value;
		}
		ticksBetweenNodes += loopLength;
	}
	int64_t valueDistance = (int64_t)rightNode->value - (int64_t)leftNode->value;
	return leftNode->value + (int32_t)(valueDistance * ticksSinceLeftNode / ticksBetweenNodes);
}

} // namespace

// ================================ NoteLandscapePattern ===================================

NoteLandscapePattern::~NoteLandscapePattern() {
	clear();
}

void NoteLandscapePattern::clear() {
	if (notes) {
		delugeDealloc(notes);
		notes = nullptr;
	}
	numNotes = 0;
	used = false;
}

bool NoteLandscapePattern::resize(int32_t newNumNotes) {
	if (notes) {
		delugeDealloc(notes);
		notes = nullptr;
	}
	numNotes = 0;
	if (newNumNotes <= 0) {
		return true;
	}
	void* mem = delugeAlloc(sizeof(LandscapeNote) * newNumNotes);
	if (!mem) {
		return false;
	}
	notes = (LandscapeNote*)mem;
	numNotes = newNumNotes;
	return true;
}

bool NoteLandscapePattern::captureFrom(InstrumentClip* clip) {
	// Count notes across every row first so we can size one flat array.
	int32_t total = 0;
	for (int32_t i = 0; i < clip->noteRows.getNumElements(); i++) {
		total += clip->noteRows.getElement(i)->notes.getNumElements();
	}
	if (!resize(total)) {
		return false;
	}
	int32_t w = 0;
	for (int32_t i = 0; i < clip->noteRows.getNumElements(); i++) {
		NoteRow* noteRow = clip->noteRows.getElement(i);
		// Row identity: getNoteRowId is the row INDEX for kits (whose NoteRow::y is unused — all -32768)
		// and the noteCode (y) for melodic clips. Using raw y would conflate every kit row.
		int32_t y = clip->getNoteRowId(noteRow, i);
		for (int32_t n = 0; n < noteRow->notes.getNumElements(); n++) {
			Note* note = noteRow->notes.getElement(n);
			LandscapeNote& ln = notes[w++];
			ln.y = y;
			ln.pos = note->pos;
			ln.length = note->getLength();
			ln.velocity = note->getVelocity();
			ln.probability = note->getProbability();
			ln.fill = note->getFill();
			ln.lift = note->getLift();
			ln.iterance = note->getIterance();
		}
	}
	used = true;
	return true;
}

// ================================== NoteLandscape ========================================

NoteLandscape::~NoteLandscape() {
	freeMorphShadow();
}

bool NoteLandscape::hasAnyPatterns() const {
	for (int32_t i = 0; i < kNoteLandscapeMaxPatterns; i++) {
		if (patterns[i].used) {
			return true;
		}
	}
	return false;
}

int32_t NoteLandscape::numUsedPatterns() const {
	int32_t n = 0;
	for (int32_t i = 0; i < kNoteLandscapeMaxPatterns; i++) {
		if (patterns[i].used) {
			n++;
		}
	}
	return n;
}

int32_t NoteLandscape::positionForRow(int32_t row) {
	// Rows span the full index range so the bottom row sits on the low rail and the top on the high.
	return (int32_t)(((int64_t)row * kNoteLandscapeIndexMax) / (kDisplayHeight - 1));
}

int32_t NoteLandscape::rowForPosition(int32_t position) {
	int32_t row =
	    (int32_t)(((int64_t)position * (kDisplayHeight - 1) + (kNoteLandscapeIndexMax / 2)) / kNoteLandscapeIndexMax);
	if (row < 0) {
		row = 0;
	}
	if (row > kDisplayHeight - 1) {
		row = kDisplayHeight - 1;
	}
	return row;
}

int32_t NoteLandscape::findFreeSlot() const {
	for (int32_t s = 0; s < kNoteLandscapeMaxPatterns; s++) {
		if (!patterns[s].used) {
			return s;
		}
	}
	return -1;
}

int32_t NoteLandscape::captureAtPosition(InstrumentClip* clip, int32_t position) {
	int32_t slot = findFreeSlot();
	if (slot < 0) {
		return -1; // all slots used
	}
	if (position < 0) {
		position = 0;
	}
	if (position > kNoteLandscapeIndexMax) {
		position = kNoteLandscapeIndexMax;
	}
	if (!patterns[slot].captureFrom(clip)) {
		return -1;
	}
	patterns[slot].position = position;
	invalidate(); // pattern set changed — force the next realize to rebuild
	return slot;
}

void NoteLandscape::repositionSlot(int32_t slot, int32_t newPosition) {
	if (slot < 0 || slot >= kNoteLandscapeMaxPatterns || !patterns[slot].used) {
		return;
	}
	if (newPosition < 0) {
		newPosition = 0;
	}
	if (newPosition > kNoteLandscapeIndexMax) {
		newPosition = kNoteLandscapeIndexMax;
	}
	patterns[slot].position = newPosition;
	invalidate();
}

int32_t NoteLandscape::usedSlotsByPosition(int32_t* out) const {
	int32_t n = 0;
	for (int32_t s = 0; s < kNoteLandscapeMaxPatterns; s++) {
		if (patterns[s].used) {
			out[n++] = s;
		}
	}
	// Insertion-sort by ascending position (n <= 8).
	for (int32_t a = 1; a < n; a++) {
		int32_t tmp = out[a];
		int32_t b = a - 1;
		while (b >= 0 && patterns[out[b]].position > patterns[tmp].position) {
			out[b + 1] = out[b];
			b--;
		}
		out[b + 1] = tmp;
	}
	return n;
}

void NoteLandscape::clearSlot(int32_t slot) {
	if (slot < 0 || slot >= kNoteLandscapeMaxPatterns) {
		return;
	}
	patterns[slot].clear();
	invalidate();
}

void NoteLandscape::cloneFrom(const NoteLandscape* other) {
	currentIndex = other->currentIndex;
	for (int32_t s = 0; s < kNoteLandscapeMaxPatterns; s++) {
		patterns[s].clear();
		const NoteLandscapePattern& src = other->patterns[s];
		if (!src.used) {
			continue;
		}
		patterns[s].position = src.position;
		patterns[s].used = true;
		if (src.numNotes > 0) {
			if (patterns[s].resize(src.numNotes)) {
				memcpy(patterns[s].notes, src.notes, sizeof(LandscapeNote) * src.numNotes);
			}
			else {
				patterns[s].used = false; // allocation failed — drop this pattern
			}
		}
	}
	invalidate();
}

void NoteLandscape::resolveStops(int32_t index, const LandscapeNote** leftNotes, int32_t* leftNum,
                                 const LandscapeNote** rightNotes, int32_t* rightNum, int32_t* tFixed) {
	// Sort used patterns by position (n <= 8, insertion sort).
	int32_t order[kNoteLandscapeMaxPatterns];
	int32_t n = 0;
	for (int32_t i = 0; i < kNoteLandscapeMaxPatterns; i++) {
		if (patterns[i].used) {
			order[n++] = i;
		}
	}
	for (int32_t a = 1; a < n; a++) {
		int32_t tmp = order[a];
		int32_t b = a - 1;
		while (b >= 0 && patterns[order[b]].position > patterns[tmp].position) {
			order[b + 1] = order[b];
			b--;
		}
		order[b + 1] = tmp;
	}

	// Build the stop list: empty rails at each end unless a pattern sits exactly on them.
	struct Stop {
		int32_t pos;
		const LandscapeNote* notes;
		int32_t num;
	};
	Stop stops[kNoteLandscapeMaxPatterns + 2];
	int32_t ns = 0;
	bool patAtLow = (n > 0 && patterns[order[0]].position <= 0);
	if (!patAtLow) {
		stops[ns++] = {0, nullptr, 0};
	}
	for (int32_t k = 0; k < n; k++) {
		NoteLandscapePattern& p = patterns[order[k]];
		stops[ns++] = {p.position, p.notes, p.numNotes};
	}
	bool patAtHigh = (n > 0 && patterns[order[n - 1]].position >= kNoteLandscapeIndexMax);
	if (!patAtHigh) {
		stops[ns++] = {kNoteLandscapeIndexMax, nullptr, 0};
	}

	// Find the bracketing pair; clamp to the ends.
	if (index <= stops[0].pos) {
		*leftNotes = *rightNotes = stops[0].notes;
		*leftNum = *rightNum = stops[0].num;
		*tFixed = 0;
		return;
	}
	if (index >= stops[ns - 1].pos) {
		*leftNotes = *rightNotes = stops[ns - 1].notes;
		*leftNum = *rightNum = stops[ns - 1].num;
		*tFixed = 0;
		return;
	}
	for (int32_t i = 0; i < ns - 1; i++) {
		if (index >= stops[i].pos && index < stops[i + 1].pos) {
			*leftNotes = stops[i].notes;
			*leftNum = stops[i].num;
			*rightNotes = stops[i + 1].notes;
			*rightNum = stops[i + 1].num;
			int32_t span = stops[i + 1].pos - stops[i].pos;
			int64_t t = ((int64_t)(index - stops[i].pos) * kNoteLandscapeIndexMax) / span;
			if (t < 0) {
				t = 0;
			}
			if (t > kNoteLandscapeIndexMax) {
				t = kNoteLandscapeIndexMax;
			}
			// Snap-plateau: scrubbing within a small band of a SAVED pattern resolves to it fully,
			// so landing "on" a pattern loads the whole thing (knob detents rarely hit it exactly).
			// Empty rails don't get a plateau (the extremes are meant to fade to nothing).
			if (stops[i].num > 0 && (index - stops[i].pos) < kNoteLandscapePlateau) {
				t = 0;
			}
			else if (stops[i + 1].num > 0 && (stops[i + 1].pos - index) < kNoteLandscapePlateau) {
				t = kNoteLandscapeIndexMax;
			}
			*tFixed = (int32_t)t;
			return;
		}
	}
	// Unreachable, but keep outputs defined.
	*leftNotes = *rightNotes = nullptr;
	*leftNum = *rightNum = 0;
	*tFixed = 0;
}

namespace {
// Apply the morph's target notes for one row, leaving the row UNTOUCHED if it already matches — so a
// surviving sounding note in an unchanged row is never cut. `target` is sorted by (y, pos); [s, e) is
// the contiguous run for this row's y (also sorted by pos, matching row.notes' order).
void applyTargetToRow(NoteRow* row, int32_t noteRowId, ModelStackWithTimelineCounter* modelStack,
                      const LandscapeNote* target, int32_t s, int32_t e) {
	int32_t targetN = e - s;
	if (targetN == row->notes.getNumElements()) {
		bool same = true;
		for (int32_t k = 0; k < targetN; k++) {
			Note* n = row->notes.getElement(k);
			const LandscapeNote& tn = target[s + k];
			if (n->pos != tn.pos || (uint8_t)n->getVelocity() != tn.velocity || n->getLength() != tn.length
			    || (uint8_t)n->getProbability() != tn.probability || (uint8_t)n->getFill() != tn.fill
			    || (uint8_t)n->getLift() != tn.lift || !(n->getIterance() == tn.iterance)) {
				same = false;
				break;
			}
		}
		if (same) {
			return; // unchanged — leave the row (and any sounding note) alone
		}
	}
	if (row->sequenced) {
		ModelStackWithNoteRow* msNR = modelStack->addNoteRow(noteRowId, row);
		row->stopCurrentlyPlayingNote(msNR);
	}
	row->notes.empty();
	for (int32_t k = s; k < e; k++) {
		int32_t i = row->notes.insertAtKey(target[k].pos);
		if (i < 0) {
			continue;
		}
		Note* note = row->notes.getElement(i);
		note->setLength(target[k].length);
		note->setVelocity(target[k].velocity);
		note->setProbability(target[k].probability);
		note->setIterance(target[k].iterance);
		note->setFill(target[k].fill);
		note->setLift(target[k].lift);
	}
}
} // namespace

// Quantize a raw index to the 0..kNoteLandscapeKnobSteps grid (the 0-50 knob readout).
static int32_t noteLandscapeKnobStep(int32_t index) {
	int32_t k =
	    (int32_t)(((int64_t)index * kNoteLandscapeKnobSteps + kNoteLandscapeIndexMax / 2) / kNoteLandscapeIndexMax);
	if (k < 0) {
		k = 0;
	}
	if (k > kNoteLandscapeKnobSteps) {
		k = kNoteLandscapeKnobSteps;
	}
	return k;
}

void NoteLandscape::buildMorphTargetAtIndex(InstrumentClip* clip, int32_t index, LandscapeNote** outTarget,
                                            int32_t* outCount) {
	int32_t knobStep = noteLandscapeKnobStep(index);
	int32_t qIndex = (int32_t)(((int64_t)knobStep * kNoteLandscapeIndexMax) / kNoteLandscapeKnobSteps);
	const LandscapeNote* L;
	const LandscapeNote* R;
	int32_t lN, rN, t;
	resolveStops(qIndex, &L, &lN, &R, &rN, &t);
	int32_t timeRadius = clip->loopLength >> kNoteLandscapeMatchTimeRadiusShift;
	if (timeRadius < 1) {
		timeRadius = 1;
	}
	assembleMorphTarget(L, lN, R, rN, t, knobStep, timeRadius, outTarget, outCount);
}

bool NoteLandscape::computeMorph(InstrumentClip* clip, LandscapeNote** outTarget, int32_t* outCount) {
	*outTarget = nullptr;
	*outCount = 0;

	// Quantize the index to the 0..kNoteLandscapeKnobSteps grid (the 0-50 knob readout) and resolve the
	// morph AT that quantized index, so the arrangement is constant within a knob step.
	int32_t knobStep = noteLandscapeKnobStep(currentIndex);
	int32_t qIndex = (int32_t)(((int64_t)knobStep * kNoteLandscapeIndexMax) / kNoteLandscapeKnobSteps);

	const LandscapeNote* L;
	const LandscapeNote* R;
	int32_t lN, rN, t;
	resolveStops(qIndex, &L, &lN, &R, &rN, &t);

	// Self-gate: same bracketing patterns at the same knob step → identical output, nothing to do.
	if (lastMorphValid && L == lastMorphL && R == lastMorphR && knobStep == lastMorphStep) {
		return false;
	}
	lastMorphL = L;
	lastMorphR = R;
	lastMorphStep = knobStep;
	lastMorphValid = true;

	int32_t timeRadius = clip->loopLength >> kNoteLandscapeMatchTimeRadiusShift;
	if (timeRadius < 1) {
		timeRadius = 1;
	}
	assembleMorphTarget(L, lN, R, rN, t, knobStep, timeRadius, outTarget, outCount);
	return true;
}

bool NoteLandscape::realize(ModelStackWithTimelineCounter* modelStack) {
	if (!hasAnyPatterns()) {
		return false;
	}
	// A saved pattern is loaded into the clip for editing — the morph must not overwrite it. The played
	// morph is kept current in the shadow by realizeIntoShadow() and swapped in for playback; the grid
	// stays on the edited pattern. invalidate() is called when editing ends so the next realize() bakes.
	if (editingSlot >= 0) {
		return false;
	}
	// We're producing a single-index morph (the index isn't automated, or we're not on the bake path).
	// Mark any prior per-position bake stale so re-enabling automation re-bakes from scratch.
	bakedAutomationValid = false;
	InstrumentClip* clip = (InstrumentClip*)modelStack->getTimelineCounter();

	LandscapeNote* target;
	int32_t targetCount;
	if (!computeMorph(clip, &target, &targetCount)) {
		return false; // morph state unchanged
	}

	// Diff each existing row against its target run; unchanged rows (and their sounding notes) are
	// left untouched. A target note for a row that no longer exists is dropped. `target` is sorted by
	// y, so each row's run is contiguous (found by scan — doesn't assume any row ordering).
	for (int32_t i = 0; i < clip->noteRows.getNumElements(); i++) {
		NoteRow* row = clip->noteRows.getElement(i);
		int32_t rowId = clip->getNoteRowId(row, i); // index for kits, y for melodic
		int32_t s = 0;
		while (s < targetCount && target[s].y < rowId) {
			s++;
		}
		int32_t e = s;
		while (e < targetCount && target[e].y == rowId) {
			e++;
		}
		applyTargetToRow(row, rowId, modelStack, target, s, e);
	}

	if (target) {
		delugeDealloc(target);
	}

	// Tell the sequencer the notes changed so it re-scans for the next event.
	clip->expectEvent();
	return true;
}

bool NoteLandscape::realizeAutomatedOutput(ModelStackWithTimelineCounter* modelStack, AutoParam* indexParam) {
	if (!hasAnyPatterns() || editingSlot >= 0 || !indexParam) {
		return false;
	}
	InstrumentClip* clip = (InstrumentClip*)modelStack->getTimelineCounter();
	int32_t loopLength = clip->loopLength;

	// One scan over all pattern note positions: build a change signature (so we only re-bake when the
	// automation actually changes — the whole point is a STATIC grid) and mark which knob steps the
	// automation visits.
	bool stepUsed[kNoteLandscapeKnobSteps + 1];
	for (int32_t k = 0; k <= kNoteLandscapeKnobSteps; k++) {
		stepUsed[k] = false;
	}
	uint32_t sig = 2166136261u; // FNV-1a over the per-position automation value
	int32_t totalNotes = 0;
	for (int32_t p = 0; p < kNoteLandscapeMaxPatterns; p++) {
		if (!patterns[p].used) {
			continue;
		}
		totalNotes += patterns[p].numNotes;
		for (int32_t k = 0; k < patterns[p].numNotes; k++) {
			int32_t v = evalAutomationAt(indexParam, (uint32_t)patterns[p].notes[k].pos, loopLength);
			sig = (sig ^ (uint32_t)v) * 16777619u;
			stepUsed[noteLandscapeKnobStep(noteLandscapeParamToIndex(v))] = true;
		}
	}

	if (bakedAutomationValid && sig == lastBakeSig) {
		return false; // automation unchanged → the baked grid is already correct
	}
	lastBakeSig = sig;
	bakedAutomationValid = true;
	lastMorphValid = false; // the single-index gate is now stale (rows hold a per-position bake)

	// Assemble the baked set: for each knob step the automation visits, compute that step's morph and
	// keep each of its notes only at the positions whose automation value selects that step. Different
	// steps contribute notes at disjoint positions — the per-position morph, baked static.
	LandscapeNote* baked = (totalNotes > 0) ? (LandscapeNote*)delugeAlloc(sizeof(LandscapeNote) * totalNotes) : nullptr;
	int32_t bakedCount = 0;
	for (int32_t K = 0; K <= kNoteLandscapeKnobSteps && baked; K++) {
		if (!stepUsed[K]) {
			continue;
		}
		int32_t qIndex = (int32_t)(((int64_t)K * kNoteLandscapeIndexMax) / kNoteLandscapeKnobSteps);
		LandscapeNote* s;
		int32_t sN;
		buildMorphTargetAtIndex(clip, qIndex, &s, &sN);
		for (int32_t i = 0; i < sN && bakedCount < totalNotes; i++) {
			int32_t v = evalAutomationAt(indexParam, (uint32_t)s[i].pos, loopLength);
			if (noteLandscapeKnobStep(noteLandscapeParamToIndex(v)) == K) {
				baked[bakedCount++] = s[i];
			}
		}
		if (s) {
			delugeDealloc(s);
		}
	}

	if (baked && bakedCount > 1) {
		std::sort(baked, baked + bakedCount, [](const LandscapeNote& a, const LandscapeNote& b) {
			return (a.y != b.y) ? (a.y < b.y) : (a.pos < b.pos);
		});
	}

	// Write the baked set into the rows (diff, like realize()).
	for (int32_t i = 0; i < clip->noteRows.getNumElements(); i++) {
		NoteRow* row = clip->noteRows.getElement(i);
		int32_t rowId = clip->getNoteRowId(row, i);
		int32_t s = 0;
		while (s < bakedCount && baked[s].y < rowId) {
			s++;
		}
		int32_t e = s;
		while (e < bakedCount && baked[e].y == rowId) {
			e++;
		}
		applyTargetToRow(row, rowId, modelStack, baked, s, e);
	}

	if (baked) {
		delugeDealloc(baked);
	}
	clip->expectEvent();
	return true;
}

void NoteLandscape::commitEdit(InstrumentClip* clip) {
	if (editingSlot < 0 || !patterns[editingSlot].used) {
		return;
	}
	patterns[editingSlot].captureFrom(clip); // storage = the live edits
	// If the edit added/removed a row (e.g. a new pitch on a synth), the shadow needs an entry for it
	// or that note can't play; refresh the shadow's row structure to mirror the clip. (Content is
	// rewritten by the next realizeIntoShadow before playback reads it.)
	if (clip->noteRows.getNumElements() != morphShadowCount) {
		captureMorphShadow(clip);
	}
	invalidate(); // morph recomputes from the updated storage on the next tick
}

void NoteLandscape::realizeIntoShadow(ModelStackWithTimelineCounter* modelStack) {
	if (editingSlot < 0 || !morphShadow) {
		return;
	}
	InstrumentClip* clip = (InstrumentClip*)modelStack->getTimelineCounter();
	LandscapeNote* target;
	int32_t targetCount;
	if (!computeMorph(clip, &target, &targetCount)) {
		return; // morph unchanged — the shadow is already current
	}
	// Rebuild every shadow row's notes from the morph target. The shadow is never the sounding buffer
	// directly (it's swapped into the rows for the scan), so a full rebuild is fine — no diff needed.
	for (int32_t i = 0; i < morphShadowCount; i++) {
		MorphShadowRow& sr = morphShadow[i];
		sr.notes.empty();
		int32_t s = 0;
		while (s < targetCount && target[s].y < sr.y) {
			s++;
		}
		for (int32_t k = s; k < targetCount && target[k].y == sr.y; k++) {
			int32_t idx = sr.notes.insertAtKey(target[k].pos);
			if (idx < 0) {
				continue;
			}
			Note* note = sr.notes.getElement(idx);
			note->setLength(target[k].length);
			note->setVelocity(target[k].velocity);
			note->setProbability(target[k].probability);
			note->setIterance(target[k].iterance);
			note->setFill(target[k].fill);
			note->setLift(target[k].lift);
		}
	}
	if (target) {
		delugeDealloc(target);
	}
	clip->expectEvent();
}

void NoteLandscape::loadPatternIntoClip(int32_t slot, ModelStackWithTimelineCounter* modelStack) {
	if (slot < 0 || slot >= kNoteLandscapeMaxPatterns || !patterns[slot].used) {
		return;
	}
	InstrumentClip* clip = (InstrumentClip*)modelStack->getTimelineCounter();
	NoteLandscapePattern& p = patterns[slot];

	// Build the pattern's notes into a target buffer sorted by (y, pos), then diff each existing row
	// against its run — identical to realize()'s materialization, but with the whole pattern present
	// (no morph). Notes on rows the clip no longer has are dropped (same as realize()).
	int32_t cap = p.numNotes;
	LandscapeNote* target = (cap > 0) ? (LandscapeNote*)delugeAlloc(sizeof(LandscapeNote) * cap) : nullptr;
	int32_t targetCount = 0;
	for (int32_t k = 0; k < p.numNotes && target; k++) {
		target[targetCount++] = p.notes[k];
	}
	if (target && targetCount > 1) {
		std::sort(target, target + targetCount, [](const LandscapeNote& a, const LandscapeNote& b) {
			return (a.y != b.y) ? (a.y < b.y) : (a.pos < b.pos);
		});
	}

	for (int32_t i = 0; i < clip->noteRows.getNumElements(); i++) {
		NoteRow* row = clip->noteRows.getElement(i);
		int32_t rowId = clip->getNoteRowId(row, i); // index for kits, y for melodic
		int32_t s = 0;
		while (s < targetCount && target[s].y < rowId) {
			s++;
		}
		int32_t e = s;
		while (e < targetCount && target[e].y == rowId) {
			e++;
		}
		applyTargetToRow(row, rowId, modelStack, target, s, e);
	}

	if (target) {
		delugeDealloc(target);
	}

	clip->expectEvent();
	// The clip's notes are now the pattern, not the morph — force the next (post-edit) realize() to
	// rebake from the resumed morph state.
	lastMorphValid = false;
}

// --- Frozen-morph shadow (edit/playback decoupling) ------------------------------------------

void NoteLandscape::freeMorphShadow() {
	if (!morphShadow) {
		return;
	}
	for (int32_t i = 0; i < morphShadowCount; i++) {
		morphShadow[i].~MorphShadowRow();
	}
	delugeDealloc(morphShadow);
	morphShadow = nullptr;
	morphShadowCount = 0;
}

void NoteLandscape::captureMorphShadow(InstrumentClip* clip) {
	freeMorphShadow();
	int32_t n = clip->noteRows.getNumElements();
	if (n <= 0) {
		return;
	}
	void* mem = delugeAlloc(sizeof(MorphShadowRow) * n);
	if (!mem) {
		return;
	}
	morphShadow = (MorphShadowRow*)mem;
	morphShadowCount = n;
	for (int32_t i = 0; i < n; i++) {
		NoteRow* row = clip->noteRows.getElement(i);
		new (&morphShadow[i]) MorphShadowRow();
		morphShadow[i].y = clip->getNoteRowId(row, i); // index for kits, y for melodic
		morphShadow[i].notes.cloneFrom(&row->notes);   // deep-copy the frozen morph notes
	}
}

NoteLandscape::MorphShadowRow* NoteLandscape::findMorphShadow(int16_t y) {
	for (int32_t i = 0; i < morphShadowCount; i++) {
		if (morphShadow[i].y == y) {
			return &morphShadow[i];
		}
	}
	return nullptr;
}

void NoteLandscape::swapInMorph(InstrumentClip* clip, MorphSwap& swap) {
	swap.count = 0;
	if (editingSlot < 0) {
		return;
	}
	for (int32_t i = 0; i < clip->noteRows.getNumElements(); i++) {
		NoteRow* row = clip->noteRows.getElement(i);
		MorphShadowRow* shadow = findMorphShadow(clip->getNoteRowId(row, i));
		if (shadow) {
			// row(edit) <-> shadow(morph): row now plays the frozen morph, edits parked in the shadow.
			row->notes.swapStateWith(&shadow->notes);
		}
		else if (swap.count < MorphSwap::kMaxUnmatched) {
			// Row created during this edit session (no frozen-morph entry) — the morph had nothing here,
			// so park its edited notes in scratch and leave the row empty for the scan.
			int32_t k = swap.count++;
			swap.rows[k] = row;
			row->notes.swapStateWith(&swap.held[k]);
		}
		// (If we somehow exceed kMaxUnmatched, that row keeps its edited notes for this tick — a benign,
		// vanishingly rare degenerate case.)
	}
}

void NoteLandscape::swapOutMorph(InstrumentClip* clip, MorphSwap& swap) {
	if (editingSlot < 0) {
		return;
	}
	for (int32_t i = 0; i < clip->noteRows.getNumElements(); i++) {
		NoteRow* row = clip->noteRows.getElement(i);
		MorphShadowRow* shadow = findMorphShadow(clip->getNoteRowId(row, i));
		if (shadow) {
			row->notes.swapStateWith(&shadow->notes); // restore: row=edit, shadow=morph
		}
	}
	// Restore the unmatched (newly-created) rows' edited notes.
	for (int32_t k = 0; k < swap.count; k++) {
		swap.rows[k]->notes.swapStateWith(&swap.held[k]);
	}
	swap.count = 0;
}

// ================================== Serialization ========================================
// Song-only: note landscapes live in the song file, never baked into a preset (mirrors midiInput).

void NoteLandscape::writeToFile(Serializer& writer) {
	if (!hasAnyPatterns()) {
		return;
	}
	writer.writeOpeningTagBeginning("noteLandscape");
	writer.writeAttributeHex("index", currentIndex, 8);
	writer.writeOpeningTagEnd();

	for (int32_t s = 0; s < kNoteLandscapeMaxPatterns; s++) {
		NoteLandscapePattern& p = patterns[s];
		if (!p.used) {
			continue;
		}
		writer.writeOpeningTagBeginning("pattern");
		writer.writeAttribute("slot", s);
		writer.writeAttributeHex("position", p.position, 8);
		writer.writeAttribute("numNotes", p.numNotes);
		writer.writeOpeningTagEnd();
		for (int32_t i = 0; i < p.numNotes; i++) {
			LandscapeNote& ln = p.notes[i];
			writer.writeOpeningTagBeginning("n");
			writer.writeAttribute("y", ln.y);
			writer.writeAttributeHex("pos", ln.pos, 8);
			writer.writeAttributeHex("len", ln.length, 8);
			writer.writeAttribute("vel", ln.velocity);
			writer.writeAttribute("prob", ln.probability);
			writer.writeAttribute("fill", ln.fill);
			writer.writeAttribute("lift", ln.lift);
			writer.writeAttribute("iter", ln.iterance.toInt());
			writer.closeTag();
		}
		writer.writeClosingTag("pattern");
	}
	writer.writeClosingTag("noteLandscape");
}

void NoteLandscape::readFromFile(Deserializer& reader) {
	char const* tagName;
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "index")) {
			currentIndex = reader.readTagOrAttributeValueHex(0);
			reader.exitTag("index");
		}
		else if (!strcmp(tagName, "pattern")) {
			int32_t slot = -1;
			int32_t position = 0;
			int32_t numNotes = 0;
			char const* inner;
			while (*(inner = reader.readNextTagOrAttributeName())) {
				if (!strcmp(inner, "slot")) {
					slot = reader.readTagOrAttributeValueInt();
					reader.exitTag("slot");
				}
				else if (!strcmp(inner, "position")) {
					position = reader.readTagOrAttributeValueHex(0);
					reader.exitTag("position");
				}
				else if (!strcmp(inner, "numNotes")) {
					numNotes = reader.readTagOrAttributeValueInt();
					// slot/position/numNotes are attributes, so they arrive before any "n" child:
					// size the slot now and append notes as they come.
					if (slot >= 0 && slot < kNoteLandscapeMaxPatterns) {
						NoteLandscapePattern& p = patterns[slot];
						p.clear();
						p.resize(numNotes > 0 ? numNotes : 0);
						p.numNotes = 0; // recount as we append
						p.position = position;
						p.used = true;
					}
					reader.exitTag("numNotes");
				}
				else if (!strcmp(inner, "n")) {
					LandscapeNote ln{};
					char const* na;
					while (*(na = reader.readNextTagOrAttributeName())) {
						if (!strcmp(na, "y")) {
							ln.y = reader.readTagOrAttributeValueInt();
							reader.exitTag("y");
						}
						else if (!strcmp(na, "pos")) {
							ln.pos = reader.readTagOrAttributeValueHex(0);
							reader.exitTag("pos");
						}
						else if (!strcmp(na, "len")) {
							ln.length = reader.readTagOrAttributeValueHex(0);
							reader.exitTag("len");
						}
						else if (!strcmp(na, "vel")) {
							ln.velocity = reader.readTagOrAttributeValueInt();
							reader.exitTag("vel");
						}
						else if (!strcmp(na, "prob")) {
							ln.probability = reader.readTagOrAttributeValueInt();
							reader.exitTag("prob");
						}
						else if (!strcmp(na, "fill")) {
							ln.fill = reader.readTagOrAttributeValueInt();
							reader.exitTag("fill");
						}
						else if (!strcmp(na, "lift")) {
							ln.lift = reader.readTagOrAttributeValueInt();
							reader.exitTag("lift");
						}
						else if (!strcmp(na, "iter")) {
							ln.iterance = Iterance::fromInt(reader.readTagOrAttributeValueInt());
							reader.exitTag("iter");
						}
						else {
							reader.exitTag(na);
						}
					}
					if (slot >= 0 && slot < kNoteLandscapeMaxPatterns) {
						NoteLandscapePattern& p = patterns[slot];
						if (p.notes && p.numNotes < numNotes) {
							p.notes[p.numNotes++] = ln;
						}
					}
					reader.exitTag("n");
				}
				else {
					reader.exitTag(inner);
				}
			}
			// A pattern with zero notes still occupies its slot (an "empty" capture).
			if (slot >= 0 && slot < kNoteLandscapeMaxPatterns && !patterns[slot].used) {
				patterns[slot].position = position;
				patterns[slot].used = true;
			}
			reader.exitTag("pattern");
		}
		else {
			reader.exitTag(tagName);
		}
	}
}
