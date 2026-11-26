#pragma once

#if defined(__ANDROID__)
namespace br::runtime {

// Configures a private, shareable development log and mirrors entries to
// Android logcat. The path is supplied by the launcher from getFilesDir().
void configureAndroidDiagnostics(const char* path);
int androidDiagnosticLogPrint(int priority, const char* tag, const char* format, ...);
int androidDiagnosticLogEvent(int priority, const char* tag, const char* event,
	const char* format, ...);
void setAndroidDiagnosticsAttemptId(const char* attemptId);

} // namespace br::runtime
#endif
