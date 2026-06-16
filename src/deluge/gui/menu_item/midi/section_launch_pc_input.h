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
#include "definitions_cxx.hpp"
#include "gui/menu_item/integer.h"
#include "gui/ui/sound_editor.h"
#include "hid/display/oled.h"
#include "io/midi/learned_midi.h"
#include "io/midi/midi_device.h"
#include "io/midi/midi_device_manager.h"
#include "model/song/song.h"
#include "util/functions.h"

class MIDICable;

namespace deluge::gui::menu_item::midi {

// "Section Launch PC Input" — a song-global device+channel binding whose incoming Program Change messages
// launch sections by index: PC value N arms section N, exactly as if that section's launch pad were pressed
// (see PlaybackHandler::programChangeReceived). Channel-only (the PC value is the section index, not part of
// the binding); display + learn mirror the MIDI-follow channel menu / Default CC Input. Like the per-section
// launchMIDICommand, the binding is per-song state (Song::sectionLaunchPCInput), not baked into presets.
class SectionLaunchPCInput final : public Integer {
public:
	using Integer::Integer;

	LearnedMIDI& getLearnedMIDI() { return currentSong->sectionLaunchPCInput; }

	// Highest selectable value. Plain MIDI channels are 0..15; values >= MIDI_CHANNEL_MPE_LOWER_ZONE (16)
	// are MPE zones, which this binding deliberately does not support (PC is a channel-voice message).
	static constexpr int32_t kHighestChannel = MIDI_CHANNEL_MPE_LOWER_ZONE - 1; // 15

	void readCurrentValue() override { this->setValue(getLearnedMIDI().channelOrZone); }
	void writeCurrentValue() override { getLearnedMIDI().channelOrZone = this->getValue(); }
	[[nodiscard]] int32_t getMaxValue() const override { return kHighestChannel; }
	bool allowsLearnMode() override { return true; }

	void drawInteger(int32_t textWidth, int32_t textHeight, int32_t yPixel) override {
		LearnedMIDI& learnedMIDI = getLearnedMIDI();
		deluge::hid::display::oled_canvas::Canvas& canvas = hid::display::OLED::main;

		yPixel = 20;

		char const* differentiationString;
		if (MIDIDeviceManager::differentiatingInputsByDevice) {
			differentiationString = l10n::get(l10n::String::STRING_FOR_INPUT_DIFFERENTIATION_ON);
		}
		else {
			differentiationString = l10n::get(l10n::String::STRING_FOR_INPUT_DIFFERENTIATION_OFF);
		}
		canvas.drawString(differentiationString, 0, yPixel, kTextSpacingX, kTextSizeYUpdated);

		yPixel += kTextSpacingY;

		char const* deviceString = l10n::get(l10n::String::STRING_FOR_FOLLOW_DEVICE_UNASSIGNED);
		if (learnedMIDI.cable) {
			deviceString = learnedMIDI.cable->getDisplayName();
		}
		canvas.drawString(deviceString, 0, yPixel, kTextSpacingX, kTextSizeYUpdated);
		deluge::hid::display::OLED::setupSideScroller(0, deviceString, kTextSpacingX, OLED_MAIN_WIDTH_PIXELS, yPixel,
		                                              yPixel + 8, kTextSpacingX, kTextSpacingY, false);

		yPixel += kTextSpacingY;

		char const* channelText;
		if (this->getValue() == MIDI_CHANNEL_NONE) {
			channelText = l10n::get(l10n::String::STRING_FOR_FOLLOW_CHANNEL_UNASSIGNED);
		}
		else {
			channelText = l10n::get(l10n::String::STRING_FOR_CHANNEL);
			char buffer[12];
			intToString(learnedMIDI.channelOrZone + 1, buffer, 1);
			canvas.drawString(buffer, kTextSpacingX * 8, yPixel, kTextSpacingX, kTextSizeYUpdated);
		}
		canvas.drawString(channelText, 0, yPixel, kTextSpacingX, kTextSizeYUpdated);
	}

	void drawValue() override {
		if (this->getValue() == MIDI_CHANNEL_NONE) {
			display->setText(l10n::get(l10n::String::STRING_FOR_NONE));
		}
		else {
			display->setTextAsNumber(this->getValue() + 1);
		}
	}

	void selectEncoderAction(int32_t offset) override {
		if (this->getValue() == MIDI_CHANNEL_NONE) {
			if (offset > 0) {
				this->setValue(0);
			}
			else if (offset < 0) {
				this->setValue(kHighestChannel);
			}
		}
		else {
			this->setValue(this->getValue() + offset);
			// Wrap to "unassigned" past either end of the plain-channel range (no MPE zones).
			if ((this->getValue() > kHighestChannel) || (this->getValue() < 0)) {
				this->setValue(MIDI_CHANNEL_NONE);
				getLearnedMIDI().clear();
				renderDisplay();
				return;
			}
		}
		Number::selectEncoderAction(offset);
	}

	void unlearnAction() override {
		this->setValue(MIDI_CHANNEL_NONE);
		getLearnedMIDI().clear();
		if (soundEditor.getCurrentMenuItem() == this) {
			renderDisplay();
		}
		else {
			display->displayPopup(l10n::get(l10n::String::STRING_FOR_UNLEARNED));
		}
	}

	// The source naturally sends Program Change (that's what drives sections), but accept any channel-voice
	// message during learn for convenience — only the device + channel are captured either way.
	void learnProgramChange(MIDICable& cable, int32_t channel, int32_t programNumber) override {
		learnFromMessage(cable, channel);
	}

	bool learnNoteOn(MIDICable& cable, int32_t channel, int32_t noteCode) override {
		return learnFromMessage(cable, channel);
	}

	void learnCC(MIDICable& cable, int32_t channel, int32_t ccNumber, int32_t value) override {
		learnFromMessage(cable, channel);
	}

	void renderDisplay() {
		if (display->haveOLED()) {
			renderUIsForOled();
		}
		else {
			drawValue();
		}
	}

private:
	// Captures the source device + channel (channel-only; the PC value is the section index, not part of the
	// binding). Mirrors how midiInput / Default CC Input are channel-learned.
	bool learnFromMessage(MIDICable& cable, int32_t channel) {
		LearnedMIDI& learnedMIDI = getLearnedMIDI();
		this->setValue(channel);
		learnedMIDI.cable = &cable;
		learnedMIDI.channelOrZone = channel;

		if (soundEditor.getCurrentMenuItem() == this) {
			renderDisplay();
		}
		else {
			display->displayPopup(l10n::get(l10n::String::STRING_FOR_LEARNED));
		}
		return true;
	}
};
} // namespace deluge::gui::menu_item::midi
