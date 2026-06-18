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

#include "model/iterance/iterance.h"
#include "model/note/note_vector.h"
#include <cstdint>

class InstrumentClip;
class NoteRow;
class AutoParam;
class ModelStackWithTimelineCounter;
class Serializer;
class Deserializer;

/// "Note Landscape": a per-clip morph between saved NOTE PATTERNS, driven by a single index value
/// (the "knob"). Sibling of ParamLandscape, but the output is the clip's whole set of notes rather
/// than a scalar. Both ends of the index default to NO notes; in between, the engine cross-fades
/// between adjacent saved patterns.
///
/// The fade is on note PRESENCE (probability), never amplitude: a note is an event, not a signal,
/// so velocity can't carry it smoothly to silence (envelope still triggers; velocity is a
/// patch-dependent mod source; velocity floor is 1). Instead every resolved note is a draw from a
/// categorical distribution over {candidate A, candidate B, nothing}, weighted by the index and
/// resolved by DETERMINISTIC DITHER (a stable per-slot threshold). A one-sided note is the
/// degenerate case where the other outcome is "nothing"; a note that slides in pitch/time is the
/// case where both outcomes are real notes and exactly one plays. This dodges the phase-
/// cancellation that bit ParamLandscape: two out-of-phase notes never sound together and cancel.

/// Full index range of the morph knob. t reaches this value to fully select the right-hand side.
constexpr int32_t kNoteLandscapeIndexMax = 0x10000;
/// Max saved patterns per clip. Mirrors ParamLandscape's interior-node cap (kMaxInteriorNodes == 4):
/// a small, fixed set keeps the morph legible and the per-tick bracket search trivial, and leaves the
/// left sidebar column (index pad + output pad + patterns) comfortably within kDisplayHeight rows.
constexpr int32_t kNoteLandscapeMaxPatterns = 4;
/// Snap band (in index units) around a saved pattern within which the morph resolves to it fully,
/// so scrubbing onto a pattern loads the whole thing rather than a near-miss partial crossfade.
constexpr int32_t kNoteLandscapePlateau = kNoteLandscapeIndexMax / 24;

/// The crossfade weight is quantized to this many levels before the per-note dither decision, so
/// the morph has a fixed set of stable, repeatable states per segment (sub-step index/reposition
/// jitter doesn't re-roll which notes appear) — and the morph only needs re-baking when the
/// quantized level changes, which makes the per-tick (mid-loop) realize cheap.
constexpr int32_t kNoteLandscapeDitherLevels = 128;

/// The morph is quantized across the WHOLE index sweep to this many steps (giving 0..N == the 0-50
/// knob readout in the UI), so the arrangement only re-rolls when the displayed number changes — one
/// stable, repeatable arrangement per knob step (less "touchy" than re-rolling every dither level).
constexpr int32_t kNoteLandscapeKnobSteps = 50;

/// How much the dither RANKING reshuffles per knob step, out of kNoteLandscapeShimmerScale. Each note
/// is ranked by a blend of a STABLE per-note value and a per-step jittered value: 0 = fully stable
/// (each note switches once and sticks — monotonic, no shimmer), scale = fully per-step (maximum
/// shimmer). In between gives notes "inertia" — they resist changing back and forth. The count that
/// has switched is always exactly the blend fraction regardless of this (rank-based selection).
constexpr int32_t kNoteLandscapeShimmerScale = 256;
constexpr int32_t kNoteLandscapeShimmer = 96;

/// The morph index is driven by an automatable unpatched param (UNPATCHED_NOTE_LANDSCAPE_INDEX)
/// whose value spans the full signed int32 range (knob bottom = INT32_MIN). These map it to/from
/// the engine's 0..kNoteLandscapeIndexMax index.
inline int32_t noteLandscapeParamToIndex(int32_t paramValue) {
	int64_t span = (int64_t)paramValue - (int64_t)INT32_MIN; // 0 .. 2^32-1
	int64_t idx = span * kNoteLandscapeIndexMax / 0xFFFFFFFFLL;
	if (idx < 0) {
		idx = 0;
	}
	if (idx > kNoteLandscapeIndexMax) {
		idx = kNoteLandscapeIndexMax;
	}
	return (int32_t)idx;
}
inline int32_t noteLandscapeIndexToParam(int32_t index) {
	if (index < 0) {
		index = 0;
	}
	if (index > kNoteLandscapeIndexMax) {
		index = kNoteLandscapeIndexMax;
	}
	return (int32_t)((int64_t)INT32_MIN + (int64_t)index * 0xFFFFFFFFLL / kNoteLandscapeIndexMax);
}

/// Matching "feel" tunable. Two notes are matching candidates iff they're within this TIME radius
/// (eligibility is time-only — no pitch/row gate, so a closed hat ↔ open hat on the same beat
/// match). Among candidates, cost is time-primary with pitch/row only a tiebreaker. A far note
/// fades independently vs "nothing".
constexpr int32_t kNoteLandscapeMatchTimeRadiusShift = 3; // time radius = loopLength >> this (loop/8)

/// A single captured note, decoupled from any live NoteRow object.
struct LandscapeNote {
	int16_t y; // NoteRow::y identity (pitch / drum row)
	int32_t pos;
	int32_t length;
	uint8_t velocity;
	uint8_t probability;
	uint8_t fill;
	uint8_t lift;
	Iterance iterance;
};

/// One saved pattern: a position on the index knob plus a snapshot of every note in the clip.
class NoteLandscapePattern {
public:
	NoteLandscapePattern() = default;
	~NoteLandscapePattern();

	void clear();
	/// Deep-copy all notes of the clip into this pattern. Returns false on allocation failure.
	bool captureFrom(InstrumentClip* clip);
	bool resize(int32_t newNumNotes);

	bool used = false;
	int32_t position = 0; // 0..kNoteLandscapeIndexMax
	int32_t numNotes = 0;
	LandscapeNote* notes = nullptr;
};

/// Per-clip note-landscape: stores the patterns and rebuilds the clip's live notes from the morph
/// at the current index.
class NoteLandscape {
public:
	NoteLandscape() = default;
	~NoteLandscape();

	bool hasAnyPatterns() const;
	int32_t numUsedPatterns() const;

	/// Capture the clip's current notes as a new pattern at an arbitrary index `position`, into the
	/// first free storage slot. Returns the slot used, or -1 if full / on allocation failure.
	int32_t captureAtPosition(InstrumentClip* clip, int32_t position);
	void clearSlot(int32_t slot);
	/// Deep-copy another landscape's index + saved patterns into this one (for clip duplication). Does
	/// NOT copy transient state (editing slot, morph shadow, gates).
	void cloneFrom(const NoteLandscape* other);
	int32_t findFreeSlot() const;
	/// Move a saved pattern to a new (clamped) index position.
	void repositionSlot(int32_t slot, int32_t newPosition);
	/// Index position a sidebar knob-map ROW (0..kDisplayHeight-1) maps to, spanning the rails. Used
	/// for quick "save at this row" placement and for rendering patterns onto the map.
	static int32_t positionForRow(int32_t row);
	/// The knob-map row a given index position falls on.
	static int32_t rowForPosition(int32_t position);
	/// Used slots ordered by ascending position; fills `out` (cap kNoteLandscapeMaxPatterns), returns count.
	int32_t usedSlotsByPosition(int32_t* out) const;

	/// Rebuild the clip's live NoteRows to BE the morph at the current index. Self-gates on the
	/// quantized morph state, so it's cheap to call every tick (mid-loop) — only rows whose notes
	/// actually change are touched, so surviving notes in unchanged rows are never cut. Returns true
	/// iff it actually changed the notes (caller can then request a re-render).
	///
	/// SUSPENDED while editingSlot >= 0: a saved pattern is loaded into the clip's notes for direct
	/// editing (the analog of Param Landscape's applyLandscapeLaneView redirecting the grid to a
	/// node's value lane). While editing, the morph must not overwrite those notes — so scrubbing the
	/// index records automation but leaves the edited grid alone.
	bool realize(ModelStackWithTimelineCounter* modelStack);
	/// When the index param is AUTOMATED, bake the per-position morph into the rows: for each note
	/// position, evaluate the index automation there and keep the morph note that the automation selects
	/// at that position. The result is STATIC (depends on the automation curve, not the live playhead),
	/// so the grid stops flickering — yet playback is identical to the live morph (each position still
	/// triggers exactly the note it does tick-by-tick). Self-gated on an automation-change signature, so
	/// it re-bakes only when the automation (or, via invalidate(), the patterns) change. Returns true
	/// iff it changed the notes.
	bool realizeAutomatedOutput(ModelStackWithTimelineCounter* modelStack, AutoParam* indexParam);
	/// Materialize a saved pattern's notes directly into the clip's NoteRows for editing (read-write),
	/// bypassing the morph. Diff-based like realize() (unchanged rows untouched). Does NOT set
	/// editingSlot — the caller owns the lane-selection state.
	void loadPatternIntoClip(int32_t slot, ModelStackWithTimelineCounter* modelStack);

	// --- Edit/playback decoupling (the analog of Param Landscape editing a node's value lane while
	// playback reads the resolved output) ---
	//
	// While a pattern is loaded for editing, the clip's live NoteRows hold the EDITABLE pattern (so all
	// the normal note-editing & rendering code works unchanged). Playback, however, must keep sounding
	// the MORPH, not the edited pattern. We snapshot the morph into a "shadow" the instant editing
	// begins, then — on each playback tick — swap that frozen morph into the live rows just for the
	// note scan and swap the edited notes back out (single-threaded, so the UI never sees the morph and
	// playback never sees the edits). realize() stays suspended throughout, so editing the grid never
	// changes what plays; scrubbing the index still records automation but the morph stays frozen until
	// editing ends.

	/// Snapshot the clip's current live notes as the morph shadow to play while a pattern is edited.
	/// Call with the live rows holding the morph (realize() just run), BEFORE loading the edit pattern.
	void captureMorphShadow(InstrumentClip* clip);
	void freeMorphShadow();
	/// While a pattern is loaded for editing, recompute the live morph for the CURRENT index into the
	/// shadow (not the live rows) so the swapped-in playback follows index scrubs/automation, while the
	/// edited grid stays put. Cheap (self-gated): a no-op when the quantized morph state is unchanged.
	void realizeIntoShadow(ModelStackWithTimelineCounter* modelStack);
	/// Fold the live edited notes back into the editing slot's storage and invalidate the morph, so the
	/// played morph reflects the edit. Refreshes the shadow's row structure if a row was added/removed.
	void commitEdit(InstrumentClip* clip);

	/// Caller-owned scratch for one playback swap (holds the edited notes swapped out of rows that have
	/// no shadow entry — rows created during this edit session). Lives across swapIn/swapOut.
	struct MorphSwap {
		static constexpr int32_t kMaxUnmatched = 8;
		NoteRow* rows[kMaxUnmatched];
		NoteVector held[kMaxUnmatched];
		int32_t count = 0;
	};
	/// Swap the frozen morph INTO the live rows (and the edited notes out) for a playback scan. O(1)
	/// per row, no allocation. swapOutMorph reverses it. Only acts while editingSlot >= 0.
	void swapInMorph(InstrumentClip* clip, MorphSwap& swap);
	void swapOutMorph(InstrumentClip* clip, MorphSwap& swap);
	/// Force the next realize() / automation bake to rebuild even if the state looks unchanged (e.g.
	/// after a pattern edit or load).
	void invalidate() {
		lastMorphValid = false;
		bakedAutomationValid = false;
	}

	void writeToFile(Serializer& writer);
	void readFromFile(Deserializer& reader);

	int32_t currentIndex = 0; // 0..kNoteLandscapeIndexMax
	/// Transient UI state (never serialized): which saved pattern is currently loaded into the clip's
	/// notes for direct editing, or -1 if the morph output is live. While >= 0, realize() is suspended
	/// so index scrubbing can record automation without clobbering the edited grid.
	int32_t editingSlot = -1;
	NoteLandscapePattern patterns[kNoteLandscapeMaxPatterns];

private:
	/// One frozen-morph note set, keyed by the live row's y, captured at edit-entry.
	struct MorphShadowRow {
		int16_t y = 0;
		NoteVector notes;
	};
	MorphShadowRow* morphShadow = nullptr;
	int32_t morphShadowCount = 0;
	MorphShadowRow* findMorphShadow(int16_t y);

	/// Resolve the bracketing pair of stops (saved patterns or empty rails) for `index`.
	void resolveStops(int32_t index, const LandscapeNote** leftNotes, int32_t* leftNum,
	                  const LandscapeNote** rightNotes, int32_t* rightNum, int32_t* tFixed);
	/// Compute the morph's target note set for `currentIndex` (categorical match + dither), sorted by
	/// (y, pos). Returns true with a freshly-allocated *outTarget (caller frees) / *outCount when the
	/// quantized morph state CHANGED since the last call; returns false (no allocation) when unchanged
	/// (self-gate), so callers keep their previous output. Shared by realize() and realizeIntoShadow().
	bool computeMorph(InstrumentClip* clip, LandscapeNote** outTarget, int32_t* outCount);
	/// Like computeMorph but at an ARBITRARY index, with no self-gate — used by the automation bake to
	/// compute the morph at each knob step the automation visits. Allocates *outTarget (caller frees).
	void buildMorphTargetAtIndex(InstrumentClip* clip, int32_t index, LandscapeNote** outTarget, int32_t* outCount);

	// Last-baked morph state, for self-gating realize(): the two bracketing patterns and the quantized
	// knob step. realize() rebuilds only when these change (or after invalidate()).
	const LandscapeNote* lastMorphL = nullptr;
	const LandscapeNote* lastMorphR = nullptr;
	int32_t lastMorphStep = -1;
	bool lastMorphValid = false;

	// Gate for the automated-index per-position bake (realizeAutomatedOutput): re-bake only when this
	// signature of the automation-at-pattern-positions changes (or invalidate() on a pattern change).
	bool bakedAutomationValid = false;
	uint32_t lastBakeSig = 0;
};
