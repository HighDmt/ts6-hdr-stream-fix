#pragma once

// Hooks IDXGIOutput1::DuplicateOutput and IDXGIOutput5::DuplicateOutput1.
// Call from a normal thread (not DllMain). Returns true if at least DuplicateOutput is hooked.
bool InstallHooks();
