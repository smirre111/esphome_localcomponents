#pragma once
// MacWireConfig.h -- the hub's wire-format switch (docs/mac-wire-format.md 12).
//
// S8 / W4: MAC wire v1 is the DEFAULT. A build that defines MAC_WIRE_V1=0 gets
// the v0 envelope path, which stays compiled until W5 deletes it. A
// compile-time switch, not a runtime dual stack: no image speaks both.
// Include this before any `#if MAC_WIRE_V1`.
#ifndef MAC_WIRE_V1
#define MAC_WIRE_V1 1
#endif
