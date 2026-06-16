#include "CppUTest/TestHarness.h"

#include "io/midi/midi_feedback_working_set.h"
#include "modulation/params/param.h"
#include <cstdint>

namespace {
using deluge::modulation::params::Kind;

// The ring only ever compares ModControllable* by value (it never dereferences them), so opaque fake
// pointers are all the tests need.
ModControllable* fakeMc(uintptr_t n) {
	return reinterpret_cast<ModControllable*>(0x1000 + n);
}
} // namespace

TEST_GROUP(MidiFeedbackWorkingSetTest){};

TEST(MidiFeedbackWorkingSetTest, recordsAndReportsTouched) {
	MidiFeedbackWorkingSet ws;
	CHECK_FALSE(ws.isTouched(fakeMc(1), Kind::PATCHED, 10));
	ws.recordTouch(fakeMc(1), Kind::PATCHED, 10);
	CHECK_TRUE(ws.isTouched(fakeMc(1), Kind::PATCHED, 10));
	// A different mod-controllable, kind, or paramId is a distinct entry.
	CHECK_FALSE(ws.isTouched(fakeMc(2), Kind::PATCHED, 10));
	CHECK_FALSE(ws.isTouched(fakeMc(1), Kind::UNPATCHED_SOUND, 10));
	CHECK_FALSE(ws.isTouched(fakeMc(1), Kind::PATCHED, 11));
}

TEST(MidiFeedbackWorkingSetTest, nullModControllableIgnored) {
	MidiFeedbackWorkingSet ws;
	ws.recordTouch(nullptr, Kind::PATCHED, 10);
	CHECK_FALSE(ws.isTouched(nullptr, Kind::PATCHED, 10));
}

TEST(MidiFeedbackWorkingSetTest, dedupsRepeatedTouch) {
	MidiFeedbackWorkingSet ws;
	for (int i = 0; i < 5; i++) {
		ws.recordTouch(fakeMc(1), Kind::PATCHED, 10);
	}
	// A re-touch occupies a single slot, so filling the rest of the ring shouldn't evict it.
	for (int i = 0; i < MidiFeedbackWorkingSet::kCapacity - 1; i++) {
		ws.recordTouch(fakeMc(2), Kind::PATCHED, i);
	}
	CHECK_TRUE(ws.isTouched(fakeMc(1), Kind::PATCHED, 10));
}

TEST(MidiFeedbackWorkingSetTest, evictsOldestWhenFull) {
	MidiFeedbackWorkingSet ws;
	// Record kCapacity + 1 distinct params; the very first (oldest) falls off, the rest are retained.
	const int n = MidiFeedbackWorkingSet::kCapacity + 1;
	for (int i = 0; i < n; i++) {
		ws.recordTouch(fakeMc(1), Kind::PATCHED, i);
	}
	CHECK_FALSE(ws.isTouched(fakeMc(1), Kind::PATCHED, 0));
	for (int i = 1; i < n; i++) {
		CHECK_TRUE(ws.isTouched(fakeMc(1), Kind::PATCHED, i));
	}
}

TEST(MidiFeedbackWorkingSetTest, reTouchMovesToFrontAndSurvivesEviction) {
	MidiFeedbackWorkingSet ws;
	const int cap = MidiFeedbackWorkingSet::kCapacity;
	// Fill the ring with params 0..cap-1 (param 0 is the oldest).
	for (int i = 0; i < cap; i++) {
		ws.recordTouch(fakeMc(1), Kind::PATCHED, i);
	}
	ws.recordTouch(fakeMc(1), Kind::PATCHED, 0);            // re-touch oldest -> moves to front
	ws.recordTouch(fakeMc(1), Kind::PATCHED, 999);          // new entry -> evicts the now-oldest (param 1)
	CHECK_TRUE(ws.isTouched(fakeMc(1), Kind::PATCHED, 0));  // survived: was moved to front
	CHECK_FALSE(ws.isTouched(fakeMc(1), Kind::PATCHED, 1)); // evicted
	CHECK_TRUE(ws.isTouched(fakeMc(1), Kind::PATCHED, 999));
}

TEST(MidiFeedbackWorkingSetTest, clearForgetsEverything) {
	MidiFeedbackWorkingSet ws;
	ws.recordTouch(fakeMc(1), Kind::PATCHED, 10);
	ws.recordTouch(fakeMc(2), Kind::MIDI, 20);
	ws.clear();
	CHECK_FALSE(ws.isTouched(fakeMc(1), Kind::PATCHED, 10));
	CHECK_FALSE(ws.isTouched(fakeMc(2), Kind::MIDI, 20));
}
