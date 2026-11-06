#pragma once

// Hooks IDXGIOutput1::DuplicateOutput and IDXGIOutput5::DuplicateOutput1.
// Call from a normal thread (not DllMain). Returns true if at least DuplicateOutput is hooked.
bool InstallHooks();

// Called by the duplication wrapper when it can't produce a converted frame. It then
// reports DXGI_ERROR_ACCESS_LOST, so TeamSpeak restarts the capture and hdrfix tries
// again. After several failures in a row, screen captures are left alone for a while.
void ReportConversionFailure(const char* reason);

// Called by the duplication wrapper after its first good frame.
void ReportConversionSuccess();
