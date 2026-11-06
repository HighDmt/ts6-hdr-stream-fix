#pragma once

// Optional troubleshooting log (verbose_log=1 in hdrfix.ini).
//
// VerboseCount() counts an event; once per second hdrfix.log gets one line with the
// count (and the latest detail text) of everything that happened in that second, e.g.
// how often TeamSpeak captured a window and what the captured pixels looked like.
// Does nothing unless verbose logging is on.
void StartVerboseLog();
bool VerboseLogOn();
void VerboseCount(const char* key, const char* detail = nullptr);
