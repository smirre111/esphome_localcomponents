// Re-export of the vendored production header at the path the .cpp expects.
// Angle-bracket include skips the current-dir search and resolves via
// -I.../local_components/lora_client, where the hub's vendored copy lives.
#pragma once
#include <ClassAWindows.h>
