// Re-export of the vendored production header at the path the .cpp expects.
// Resolves via -I.../local_components/lora_client, where the hub's vendored
// copy lives. See FrameCrypto.h here for the same pattern and the reasoning.
#pragma once
#include <Scheduler.h>
