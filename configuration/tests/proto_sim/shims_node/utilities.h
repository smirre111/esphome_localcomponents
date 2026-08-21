#pragma once
// CmdDispatcher.cpp #includes "utilities.h" but the only symbols it uses
// from there go through SystemCtrl / MotorCtrl methods; on host, an empty
// shim suffices.
