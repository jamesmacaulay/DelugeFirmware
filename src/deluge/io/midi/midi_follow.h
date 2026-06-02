/*
 * Copyright (c) 2024 Sean Ditny
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
#include "model/global_effectable/global_effectable.h"
#include "modulation/params/param.h"
#include "storage/storage_manager.h"
#include "util/containers.h"
#include <cstdint>

class AudioClip;
class InstrumentClip;
class Clip;
class Kit;
class Drum;
class ModelStack;
class ModelStackWithThreeMainThings;
class ModelStackWithAutoParam;
enum class MIDIMatchType;

namespace params = deluge::modulation::params;

/// One addressable kit row, resolved from the song's kits for midi-follow dispatch.
struct MidiFollowKitRow {
	Kit* kit;
	InstrumentClip* clip;
	Drum* drum;
};

class MidiFollow final {
public:
	MidiFollow();
	void readDefaultsFromFile();

	ModelStackWithAutoParam* getModelStackWithParam(ModelStackWithTimelineCounter* modelStackWithTimelineCounter,
	                                                Clip* clip, int32_t soundParamId, int32_t globalParamId,
	                                                bool displayError = true, bool forceKitAffectEntire = false,
	                                                Drum* targetKitRowDrum = nullptr);
	void noteMessageReceived(MIDICable& cable, bool on, int32_t channel, int32_t note, int32_t velocity,
	                         bool* doingMidiThru, bool shouldRecordNotesNowNow, ModelStack* modelStack);
	Output* sendNoteToClip(MIDICable& cable, Clip* clip, MIDIMatchType match, bool on, int32_t channel, int32_t note,
	                       int32_t velocity, bool* doingMidiThru, bool shouldRecordNotesNowNow, ModelStack* modelStack,
	                       bool updateClipForLastNoteReceived = true, Drum* targetDrum = nullptr);
	void midiCCReceived(MIDICable& cable, uint8_t channel, uint8_t ccNumber, uint8_t ccValue, bool* doingMidiThru,
	                    ModelStack* modelStack);
	void pitchBendReceived(MIDICable& cable, uint8_t channel, uint8_t data1, uint8_t data2, bool* doingMidiThru,
	                       ModelStack* modelStack);
	void aftertouchReceived(MIDICable& cable, int32_t channel, int32_t value, int32_t noteCode, bool* doingMidiThru,
	                        ModelStack* modelStack);

	void clearStoredClips();
	void removeClip(Clip* clip);

	// midi CC mappings
	int32_t getCCFromParam(deluge::modulation::params::Kind paramKind, int32_t paramID);
	bool isGlobalEffectableContext();

	std::array<uint8_t, kMaxMIDIValue + 1> ccToSoundParam;
	std::array<uint8_t, kMaxMIDIValue + 1> ccToGlobalParam;
	std::array<uint8_t, params::UNPATCHED_START + params::UNPATCHED_SOUND_MAX_NUM> soundParamToCC;
	std::array<uint8_t, params::UNPATCHED_GLOBAL_MAX_NUM> globalParamToCC;

	int32_t previousKnobPos[kMaxMIDIValue + 1];
	uint32_t timeLastCCSent[kMaxMIDIValue + 1];
	uint32_t timeAutomationFeedbackLastSent;

	// public so it can be called from View::sendMidiFollowFeedback
	MIDIFollowChannelType getChannelTypeForFeedback();
	void sendCCWithoutModelStackForMidiFollowFeedback(int32_t channel, bool isAutomation = false);
	void sendCCForMidiFollowFeedback(int32_t channel, int32_t ccNumber, int32_t knobPos);

	void handleReceivedCC(ModelStackWithTimelineCounter& modelStack, Clip* clip, int32_t ccNumber, int32_t ccValue,
	                      bool forceKitAffectEntire = false, Drum* targetKitRowDrum = nullptr);

private:
	// initialize
	void init();
	void initState();
	void clearMappings();
	void initDefaultMappings();

	// note recieved
	Output* noteMessageReceivedForSelectedOrActiveClip(MIDICable& cable, bool on, int32_t channel, int32_t note,
	                                                   int32_t velocity, bool* doingMidiThru,
	                                                   bool shouldRecordNotesNowNow, ModelStack* modelStack);
	void noteMessageReceivedForSpecificTrack(MIDICable& cable, bool on, int32_t channel, int32_t note, int32_t velocity,
	                                         bool* doingMidiThru, bool shouldRecordNotesNowNow, ModelStack* modelStack,
	                                         Output* specific_track, int32_t specific_track_index);
	void noteMessageReceivedForKitRow(MIDICable& cable, bool on, int32_t channel, int32_t note, int32_t velocity,
	                                  bool* doingMidiThru, bool shouldRecordNotesNowNow, ModelStack* modelStack,
	                                  InstrumentClip* clip, Drum* drum, int32_t kit_row_index);
	// cc received
	Output* midiCCReceivedForSelectedOrActiveClip(MIDICable& cable, uint8_t channel, uint8_t ccNumber, uint8_t ccValue,
	                                              bool* doingMidiThru, ModelStack* modelStack);
	void midiCCReceivedForSpecificTrack(MIDICable& cable, uint8_t channel, uint8_t ccNumber, uint8_t ccValue,
	                                    bool* doingMidiThru, ModelStack* modelStack, Output* specific_track,
	                                    int32_t specific_track_index);
	void midiCCReceivedForKitRow(MIDICable& cable, uint8_t channel, uint8_t ccNumber, uint8_t ccValue,
	                             bool* doingMidiThru, ModelStack* modelStack, InstrumentClip* clip, Drum* drum,
	                             int32_t kit_row_index);

	// pitch bend received
	Output* pitchBendReceivedForSelectedOrActiveClip(MIDICable& cable, uint8_t channel, uint8_t data1, uint8_t data2,
	                                                 bool* doingMidiThru, ModelStack* modelStack);
	void pitchBendReceivedForSpecificTrack(MIDICable& cable, uint8_t channel, uint8_t data1, uint8_t data2,
	                                       bool* doingMidiThru, ModelStack* modelStack, Output* specific_track,
	                                       int32_t specific_track_index);
	void pitchBendReceivedForKitRow(MIDICable& cable, uint8_t channel, uint8_t data1, uint8_t data2,
	                                bool* doingMidiThru, ModelStack* modelStack, Kit* kit, InstrumentClip* clip,
	                                Drum* drum, int32_t kit_row_index);

	// after touch received
	Output* aftertouchReceivedForSelectedOrActiveClip(MIDICable& cable, int32_t channel, int32_t value,
	                                                  int32_t noteCode, bool* doingMidiThru, ModelStack* modelStack);
	void aftertouchReceivedForSpecificTrack(MIDICable& cable, int32_t channel, int32_t value, int32_t noteCode,
	                                        bool* doingMidiThru, ModelStack* modelStack, Output* specific_track,
	                                        int32_t specific_track_index);
	void aftertouchReceivedForKitRow(MIDICable& cable, int32_t channel, int32_t value, bool* doingMidiThru,
	                                 ModelStack* modelStack, Kit* kit, InstrumentClip* clip, Drum* drum,
	                                 int32_t kit_row_index);

	Clip* getSelectedOrActiveClip();
	Clip* getSelectedClip();
	Clip* getActiveClip(ModelStack* modelStack);
	[[nodiscard]] const size_t getTrackCount() const;
	Output* getTrackFromIndex(uint32_t trackIndex, uint32_t maxTrack);
	// True if any CHANNEL KIT ROW config has a channel learned. Used to skip kit-row enumeration entirely
	// (and its output-list walk) on every incoming MIDI message when the feature isn't in use.
	bool anyKitRowLearned();
	// Enumerate the song's addressable kit rows (for the CHANNEL KIT ROW configs) into `out`, in the same
	// order as CHANNEL TRACK numbering and bottom-to-top within each kit. Returns the count (<= maxRows).
	// Done once per incoming MIDI message so the output list isn't re-walked per row. Returns 0 immediately
	// when no kit-row config is learned, so the walk costs nothing for songs/users not using the feature.
	size_t enumerateKitRows(MidiFollowKitRow* out, size_t maxRows);

	// get model stack with auto param for midi follow cc-param control
	ModelStackWithAutoParam* getModelStackWithParamForSong(ModelStackWithThreeMainThings* modelStackWithThreeMainThings,
	                                                       int32_t soundParamId, int32_t globalParamId);
	ModelStackWithAutoParam* getModelStackWithParamForClip(ModelStackWithTimelineCounter* modelStackWithTimelineCounter,
	                                                       Clip* clip, int32_t soundParamId, int32_t globalParamId,
	                                                       bool forceKitAffectEntire = false,
	                                                       Drum* targetKitRowDrum = nullptr);
	ModelStackWithAutoParam*
	getModelStackWithParamForSynthClip(ModelStackWithTimelineCounter* modelStackWithTimelineCounter, Clip* clip,
	                                   int32_t soundParamId, int32_t globalParamId);
	ModelStackWithAutoParam*
	getModelStackWithParamForKitClip(ModelStackWithTimelineCounter* modelStackWithTimelineCounter, Clip* clip,
	                                 int32_t soundParamId, int32_t globalParamId, bool forceKitAffectEntire = false,
	                                 Drum* targetKitRowDrum = nullptr);
	ModelStackWithAutoParam*
	getModelStackWithParamForAudioClip(ModelStackWithTimelineCounter* modelStackWithTimelineCounter, Clip* clip,
	                                   int32_t soundParamId, int32_t globalParamId);
	void displayParamControlError(int32_t soundParamId, int32_t globalParamId);

	MIDIMatchType checkMidiFollowMatch(MIDICable& cable, uint8_t channel);
	MIDIMatchType checkMidiFollowMatchForSpecificTrack(MIDICable& cable, uint8_t channel, int32_t specific_track_index);
	MIDIMatchType checkMidiFollowMatchForKitRow(MIDICable& cable, uint8_t channel, int32_t kit_row_index);
	bool isFeedbackEnabled();

	// saving
	void writeDefaultsToFile();
	void writeDefaultMappingsToFile();

	// loading
	bool successfullyReadDefaultsFromFile;
	void readDefaultMappingsFromFile(Deserializer& reader);
};

extern MidiFollow midiFollow;
