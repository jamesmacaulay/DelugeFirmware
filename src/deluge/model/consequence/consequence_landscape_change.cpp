/*
 * Copyright © 2017-2023 Synthstrom Audible Limited
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

#include "model/consequence/consequence_landscape_change.h"
#include "memory/general_memory_allocator.h"
#include "modulation/automation/param_landscape.h"
#include "modulation/params/param_collection.h"
#include <new>

ConsequenceLandscapeChange::ConsequenceLandscapeChange(ModelStackWithAutoParam const* modelStackToCapture) {
	type = Consequence::LANDSCAPE_CHANGE;
	memcpy(modelStackMemory, modelStackToCapture, sizeof(ModelStackWithParamId));

	AutoParam* param = modelStackToCapture->autoParam;

	indexState.value = param->currentValue;
	indexState.nodes.cloneFrom(&param->nodes);
	indexState.landscapeNodeIndex = -1;

	landscape = nullptr;
	if (param->landscape) {
		void* landscapeMemory = GeneralMemoryAllocator::get().allocLowSpeed(sizeof(ParamLandscape));
		if (landscapeMemory) {
			landscape = new (landscapeMemory) ParamLandscape();
			landscape->cloneFrom(param->landscape, true);
		}
	}
}

ConsequenceLandscapeChange::~ConsequenceLandscapeChange() {
	if (landscape) {
		landscape->~ParamLandscape();
		delugeDealloc(landscape);
		landscape = nullptr;
	}
}

Error ConsequenceLandscapeChange::revert(TimeType time, ModelStack* modelStackWithSong) {
	// Swap stored and live state — works in both undo and redo directions, like
	// ConsequenceParamChange.
	modelStack.paramCollection->remotelySwapLandscapeState(&indexState, &landscape, &modelStack);
	return Error::NONE;
}
