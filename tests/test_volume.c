// Unit tests for volume.c's pure logic (vol_icon_name's mute/level
// thresholds) -- no GTK init, no Wayland, no wpctl/pactl process.
// #includes the plugin source directly to reach its `static` functions
// without changing their visibility for production code; this file
// supplies its own main() (volume.c has none), so nothing conflicts.
//
// update_bar/read_volume/read_target/rebuild_popover/device_list aren't
// unit-testable this way: update_bar unconditionally touches real GTK
// widgets, and read_target/device_list spawn real wpctl/pactl processes.
#include "../src/volume.c"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
	else { printf("ok - %s\n", msg); } \
} while (0)
#define CHECK_STR(a, b, msg) CHECK(strcmp((a), (b)) == 0, msg)

int main(void) {
	Inst s = {0};

	s.vol = 0.5; s.muted = 1;
	CHECK_STR(vol_icon_name(&s), "vol-mute.svg", "vol_icon_name: muted always shows the mute icon regardless of level");

	s.muted = 0; s.vol = 0.0;
	CHECK_STR(vol_icon_name(&s), "vol-mute.svg", "vol_icon_name: unmuted but zero volume also shows the mute icon");

	s.vol = 0.001;
	CHECK_STR(vol_icon_name(&s), "vol-mute.svg", "vol_icon_name: at-the-threshold near-zero volume still shows mute");

	s.vol = 0.01;
	CHECK_STR(vol_icon_name(&s), "vol-low.svg", "vol_icon_name: just above the near-zero threshold is low");

	s.vol = 0.33;
	CHECK_STR(vol_icon_name(&s), "vol-low.svg", "vol_icon_name(0.33) is still low (boundary)");
	s.vol = 0.34;
	CHECK_STR(vol_icon_name(&s), "vol-med.svg", "vol_icon_name(0.34) crosses into medium (boundary)");
	s.vol = 0.66;
	CHECK_STR(vol_icon_name(&s), "vol-med.svg", "vol_icon_name(0.66) is still medium (boundary)");
	s.vol = 0.67;
	CHECK_STR(vol_icon_name(&s), "vol-high.svg", "vol_icon_name(0.67) crosses into high (boundary)");
	s.vol = 1.0;
	CHECK_STR(vol_icon_name(&s), "vol-high.svg", "vol_icon_name(1.0) is high");

	printf("----\n%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
