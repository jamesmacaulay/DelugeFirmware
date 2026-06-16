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
#include "definitions_cxx.hpp"
#include "model/action/action.h"
#include "modulation/params/param_node_vector.h"
#include "storage/storage_manager.h"
#include <cstdint>

class InstrumentClip;
class ParamNode;
class Action;
class ParamCollection;
class Sound;
class CopiedParamAutomation;
class TimelineCounter;
class Clip;
class ModelStackWithAutoParam;
class ParamLandscape;

// For backing up a snapshot
class AutoParamState {
public:
	ParamNodeVector nodes;
	int32_t value;
	/// >= 0 when this snapshot belongs to a transformation-space node lane of the param (the
	/// lane's index within the landscape); -1 for the param's own (index) lane.
	int32_t landscapeNodeIndex = -1;
};

struct StolenParamNodes;

class AutoParam {
public:
	AutoParam();
	~AutoParam();
	void init();

	void setCurrentValueInResponseToUserInput(int32_t value, ModelStackWithAutoParam const* modelStack,
	                                          bool shouldLogAction = true, int32_t livePos = -1,
	                                          bool mayDeleteNodesInLinearRun = true, bool isMPE = false);
	int32_t processCurrentPos(ModelStackWithAutoParam const* modelStack, bool reversed, bool didPinpong,
	                          bool mayInterpolate = true, bool mustUpdateValueAtEveryNode = false,
	                          bool notifyChanges = true);
	void setValueForRegion(uint32_t pos, uint32_t length, int32_t value, ModelStackWithAutoParam const* modelStack,
	                       ActionType actionType = ActionType::NOTE_EDIT);
	void setValuePossiblyForRegion(int32_t value, ModelStackWithAutoParam const* modelStack, int32_t pos,
	                               int32_t length, bool mayDeleteNodesInLinearRun = true);
	int32_t getValueAtPos(uint32_t pos, ModelStackWithAutoParam const* modelStack, bool reversed = false);
	/// tick the interolator by a number of samples - used for internal synths
	bool tickSamples(int32_t numSamples);
	/// tick the interpolator by a number of ticks - used for midi
	bool tickTicks(int32_t numTicks);
	void setPlayPos(uint32_t pos, ModelStackWithAutoParam const* modelStack, bool reversed, bool notifyChanges = true);
	bool grabValueFromPos(uint32_t pos, ModelStackWithAutoParam const* modelStack);
	void generateRepeats(uint32_t oldLength, uint32_t newLength, bool shouldPingpong);
	void cloneFrom(AutoParam* otherParam, bool copyAutomation);
	Error beenCloned(bool copyAutomation, int32_t reverseDirectionWithLength);
	void copyOverridingFrom(AutoParam* otherParam);
	void trimToLength(uint32_t newLength, Action* action, ModelStackWithAutoParam const* modelStack);
	void deleteAutomation(Action* action, ModelStackWithAutoParam const* modelStack, bool shouldNotify = true);
	void deleteAutomationBasicForSetup();
	void writeToFile(Serializer& writer, bool writeAutomation, int32_t* valueForOverride = nullptr);
	Error readFromFile(Deserializer& reader, int32_t readAutomationUpToPos);
	bool containsSomething(uint32_t neutralValue = 0);
	static bool containedSomethingBefore(bool wasAutomatedBefore, uint32_t valueBefore, uint32_t neutralValue = 0);
	void shiftValues(int32_t offset);
	void shiftParamVolumeByDB(float offset);
	void shiftHorizontally(int32_t amount, int32_t effectiveLength);
	void swapState(AutoParamState* state, ModelStackWithAutoParam const* modelStack);
	void copy(int32_t startPos, int32_t endPos, CopiedParamAutomation* copiedParamAutomation, bool isPatchCable,
	          ModelStackWithAutoParam const* modelStack);
	void paste(int32_t startPos, int32_t endPos, float scaleFactor, ModelStackWithAutoParam const* modelStack,
	           CopiedParamAutomation* copiedParamAutomation, bool isPatchCable);
	Error makeInterpolationGoodAgain(int32_t clipLength, int32_t quantizationRShift);
	void transposeCCValuesToChannelPressureValues();
	void deleteTime(int32_t startPos, int32_t lengthToDelete, ModelStackWithAutoParam* modelStack);
	void insertTime(int32_t pos, int32_t lengthToInsert);
	void appendParam(AutoParam* otherParam, int32_t oldLength, int32_t reverseThisRepeatWithLength,
	                 bool pingpongingGenerally);
	void nudgeNonInterpolatingNodesAtPos(int32_t pos, int32_t offset, int32_t lengthBeforeLoop, Action* action,
	                                     ModelStackWithAutoParam const* modelStack);
	void stealNodes(ModelStackWithAutoParam const* modelStack, int32_t pos, int32_t regionLength, int32_t loopLength,
	                Action* action, StolenParamNodes* stolenNodeRecord = nullptr);
	void insertStolenNodes(ModelStackWithAutoParam const* modelStack, int32_t pos, int32_t regionLength,
	                       int32_t loopLength, Action* action, StolenParamNodes* stolenNodeRecord);
	void moveRegionHorizontally(ModelStackWithAutoParam const* modelStack, int32_t pos, int32_t length, int32_t offset,
	                            int32_t lengthBeforeLoop, Action* action);
	void deleteNodesWithinRegion(ModelStackWithAutoParam const* modelStack, int32_t pos, int32_t length);
	/// @brief Make sure the value of this AutoParam will be \p value at time \p pos.
	///
	/// If no node exists at the provided position, a new node will be created.
	///
	/// @param[in] pos Position to modify.
	/// @param[in] value Value to use.
	/// @param[in] shouldInterpolate Sets \ref "ParamNode::interpolated" on the created or modified node.
	///
	/// @return the index in to \ref nodes of the modified node.
	int32_t setNodeAtPos(int32_t pos, int32_t value, bool shouldInterpolate);
	int32_t homogenizeRegion(ModelStackWithAutoParam const* modelStack, int32_t startPos, int32_t length,
	                         int32_t startValue, bool interpolateLeftNode, bool interpolateRightNode,
	                         int32_t effectiveLength, bool reversed, int32_t posAtWhichClipWillCut = 2147483647);
	int32_t getDistanceToNextNode(ModelStackWithAutoParam const* modelStack, int32_t pos, bool reversed);
	void setCurrentValueWithNoReversionOrRecording(ModelStackWithAutoParam const* modelStack, int32_t value);

	/// The value the rest of the engine should hear. When a transformation space (landscape)
	/// exists, currentValue acts as the index into it and this returns the mapped output;
	/// otherwise it's just currentValue (stock behaviour, and the only cost is this null check).
	inline int32_t getCurrentValue() { return landscape ? landscapeOutput() : currentValue; }

	/// currentValue without the landscape mapping — the "index" in transformation-space terms.
	/// Use for code that edits or compares against the stored value/automation itself.
	inline int32_t getCurrentIndexValue() { return currentValue; }

	int32_t landscapeOutput();
	void freeLandscape();

	/// Lazily allocate the landscape. Returns nullptr on allocation failure.
	ParamLandscape* getOrCreateLandscape();

	/// The transformation-space creation op: insert a landscape node at nodePosition (sorted;
	/// position must be free; up to kMaxInteriorNodes). An automated index lane MOVES into the
	/// new node's lane; an unautomated one pins the current output there instead (an empty,
	/// sculptable node — playback-neutral). The index then parks at the node's position.
	/// slotOut (optional) receives the inserted node's index.
	Error moveAutomationIntoLandscapeNode(ModelStackWithAutoParam const* modelStack, int32_t nodePosition,
	                                      int32_t* slotOut = nullptr);

	/// OUTPUT-view save: bake the current output curve (union-of-events, like flatten) into a
	/// NEW node's lane at nodePosition, then clear the index automation and park the index
	/// there — playback-neutral: T(position) = the captured curve = what was already playing.
	Error saveOutputAsLandscapeNode(ModelStackWithAutoParam const* modelStack, int32_t nodePosition,
	                                int32_t* slotOut = nullptr);

	/// Discard the landscape; the index lane becomes the audible value again (audible jump is
	/// expected — this is a deliberate, destructive gesture). No-op without a landscape.
	void deleteLandscape(ModelStackWithAutoParam const* modelStack);

	/// Delete one saved position (node) from the landscape; deleting the last one frees the
	/// whole landscape. Audible jump expected if the index was parked on it.
	Error deleteLandscapeNode(ModelStackWithAutoParam const* modelStack, int32_t slot);

	/// "Load" a saved position: clear the index automation and park the index on the slot's
	/// position, so its lane plays verbatim. The preset-recall gesture — deliberately audible.
	Error parkIndexAtLandscapeNode(ModelStackWithAutoParam const* modelStack, int32_t slot);

	/// Clear the index automation and park the index at an arbitrary value (the LOAD+map-pad
	/// performance jump). Audible by design.
	void parkIndexAtPosition(ModelStackWithAutoParam const* modelStack, int32_t newIndexValue);

	/// SAVE onto an existing slot: replace its lane with an exact capture of the playing
	/// output, then park the index on it (playback-neutral).
	Error overwriteLandscapeNodeWithOutput(ModelStackWithAutoParam const* modelStack, int32_t slot);

	/// Bake the mapped output over time into the index lane (sampled at the union of index-lane
	/// and node-lane event positions), then discard the landscape. The inverse of
	/// moveAutomationIntoLandscapeNode for sharing/portability.
	Error flattenLandscape(ModelStackWithAutoParam const* modelStack);

	/// Output (through the landscape, if any) at a timeline position. For rendering/flattening.
	int32_t landscapeOutputAtPos(uint32_t pos, ModelStackWithAutoParam const* modelStack);

	/// OUTPUT-view copy: flatten the landscape output over [startPos, endPos) into the
	/// clipboard — "bounce automation to anywhere".
	void copyLandscapeOutput(int32_t startPos, int32_t endPos, CopiedParamAutomation* copiedParamAutomation,
	                         bool isPatchCable, ModelStackWithAutoParam const* modelStack);

	/// Whether the engine must keep ticking this param: its own lane is automated, or a
	/// landscape node lane is. This — not isAutomated() — is what should gate the
	/// whichParamsAreAutomated summary bits.
	bool needsTicking();

	/// Own interpolation or any landscape node lane's. Gates whichParamsAreInterpolating.
	bool hasActiveInterpolation();

	/// Zero own and landscape-lane interpolation increments (playback ended).
	void stopAllInterpolation();
	int32_t getValuePossiblyAtPos(int32_t pos, ModelStackWithAutoParam* modelStack);
	void notifyPingpongOccurred();

	inline void setCurrentValueBasicForSetup(int32_t value) { currentValue = value; }

	inline bool isAutomated() { return (nodes.getNumElements()); }

	inline void cancelOverriding() { // Will also cancel "latching".
		renewedOverridingAtTime = 0;
	}

	/// The nodes that make up this parameter. If empty, \ref currentValue should be used.
	ParamNodeVector nodes;

	/// Current value of the AutoParam. Updated by several functions. With a landscape present
	/// this is the *index*; consumers wanting the audible value must use getCurrentValue().
	int32_t currentValue;

	/// Optional transformation space. nullptr (the norm) = identity = stock behaviour. Owned;
	/// freed in the destructor. Node-lane AutoParams inside it must keep this nullptr.
	ParamLandscape* landscape = nullptr;
	int32_t valueIncrementPerHalfTick;
	uint32_t renewedOverridingAtTime; // If 0, it's off. If 1, it's latched until we hit some nodes / automation

	// "Latching" happens when you start recording values, but then stops if you arrive at any pre-existing values. So
	// it only works in empty stretches of time.

private:
	/// Capture the playing output as an exact node list (morphed breakpoints when parked in a
	/// sliding segment; union-of-events otherwise). Shared by save/overwrite ops.
	Error bakeOutputNodeList(ParamNodeVector* dest, ModelStackWithAutoParam const* modelStack);
	/// Sorted-insert an (empty) landscape node slot at nodePosition: lazy-create, cap and
	/// occupied-position checks, swap-based slot shifting. On success *slotOut = the new slot.
	Error insertLandscapeNodeSlot(int32_t nodePosition, int32_t* slotOut);
	/// Advance any automated landscape node lanes to the current play pos, fold their
	/// next-event distances into ticksTilNextEvent, and issue one output-space notification if
	/// the mapped output moved. Called from processCurrentPos on every path.
	int32_t processLandscapeLanesCurrentPos(ModelStackWithAutoParam const* modelStack, bool reversed, bool didPingpong,
	                                        bool mayInterpolate, int32_t ticksTilNextEvent);
	bool deleteRedundantNodeInLinearRun(int32_t lastNodeInRunI, int32_t effectiveLength,
	                                    bool mayLoopAroundBackToEnd = true);
	void setupInterpolation(ParamNode* nextNode, int32_t effectiveLength, int32_t currentPos, bool reversed);
	void homogenizeRegionTestSuccess(int32_t pos, int32_t regionEnd, int32_t startValue, bool interpolateStart,
	                                 bool interpolateEnd);
	void deleteNodesBeyondPos(int32_t pos);
};
