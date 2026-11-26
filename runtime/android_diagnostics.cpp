#include "android_diagnostics.h"

#include <android/log.h>

#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

namespace br::runtime {
namespace {
std::mutex gLogMutex;
int gLogFd = -1;
std::string gLogPath;
std::string gAttemptId;
const auto gLogStart = std::chrono::steady_clock::now();

char priorityLetter(int priority)
{
	switch (priority) {
	case ANDROID_LOG_VERBOSE: return 'V';
	case ANDROID_LOG_DEBUG: return 'D';
	case ANDROID_LOG_INFO: return 'I';
	case ANDROID_LOG_WARN: return 'W';
	case ANDROID_LOG_ERROR: return 'E';
	case ANDROID_LOG_FATAL: return 'F';
	default: return '?';
	}
}

size_t appendJsonString(char* output, size_t capacity, size_t offset, const char* value)
{
	if (offset >= capacity) return offset;
	output[offset++] = '"';
	for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value ? value : ""); *p; ++p) {
		if (offset + 8u >= capacity) break;
		switch (*p) {
		case '"': output[offset++] = '\\'; output[offset++] = '"'; break;
		case '\\': output[offset++] = '\\'; output[offset++] = '\\'; break;
		case '\n': output[offset++] = '\\'; output[offset++] = 'n'; break;
		case '\r': output[offset++] = '\\'; output[offset++] = 'r'; break;
		case '\t': output[offset++] = '\\'; output[offset++] = 't'; break;
		default:
			if (*p < 0x20u) {
				const int written = std::snprintf(output + offset, capacity - offset, "\\u%04x", *p);
				if (written > 0) offset += static_cast<size_t>(written);
			} else output[offset++] = static_cast<char>(*p);
		}
	}
	output[offset++] = '"';
	return offset;
}

bool writeAll(int fd, const char* bytes, size_t length)
{
	while (length) {
		const ssize_t written = ::write(fd, bytes, length);
		if (written < 0) return false;
		bytes += written;
		length -= static_cast<size_t>(written);
	}
	return true;
}

int logEventV(int priority, const char* tag, const char* event,
	const char* format, va_list args)
{
	char message[4096]{};
	const int length = std::vsnprintf(message, sizeof(message), format ? format : "", args);
	if (length < 0) message[0] = '\0';
	const char* eventName = event && *event ? event : "native_log";
	const char* eventTag = tag && *tag ? tag : "BattleRealmsNative";
	const auto now = std::chrono::steady_clock::now();
	const double monotonicSeconds = std::chrono::duration<double>(now - gLogStart).count();
	const auto wall = std::chrono::system_clock::now();
	const auto epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(wall.time_since_epoch()).count();
	const time_t epochSeconds = static_cast<time_t>(epochMs / 1000);
	struct tm utc{};
	gmtime_r(&epochSeconds, &utc);
	char timestamp[40]{};
	std::snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
		utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min,
		utc.tm_sec, static_cast<long long>(epochMs % 1000));
	const pid_t pid = getpid();
	const long tid = static_cast<long>(syscall(SYS_gettid));
	std::string attempt;
	int fileError = 0;
	{
		std::lock_guard<std::mutex> lock(gLogMutex);
		attempt = gAttemptId;
		char record[6144]{};
		size_t used = 0u;
		used += static_cast<size_t>(std::snprintf(record + used, sizeof(record) - used,
			"{\"ts_utc\":\"%s\",\"mono_s\":%.3f,\"level\":\"%c\",\"tag\":",
			timestamp, monotonicSeconds, priorityLetter(priority)));
		used = appendJsonString(record, sizeof(record), used, eventTag);
		used += static_cast<size_t>(std::snprintf(record + used, sizeof(record) - used,
			",\"event\":"));
		used = appendJsonString(record, sizeof(record), used, eventName);
		used += static_cast<size_t>(std::snprintf(record + used, sizeof(record) - used,
			",\"pid\":%d,\"tid\":%ld,\"attempt_id\":", static_cast<int>(pid), tid));
		used = appendJsonString(record, sizeof(record), used, attempt.c_str());
		used += static_cast<size_t>(std::snprintf(record + used, sizeof(record) - used,
			",\"message\":"));
		used = appendJsonString(record, sizeof(record), used, message);
		if (used + 2u < sizeof(record)) {
			record[used++] = '}';
			record[used++] = '\n';
			if (gLogFd >= 0 && !writeAll(gLogFd, record, used)) fileError = errno;
		}
	}
	__android_log_print(priority, eventTag, "event=%s pid=%d tid=%ld attempt=%s %s",
		eventName, static_cast<int>(pid), tid, attempt.empty() ? "none" : attempt.c_str(), message);
	if (fileError)
		__android_log_print(ANDROID_LOG_ERROR, "BattleRealmsNative",
			"diagnostics append failed errno=%d pid=%d tid=%ld attempt=%s",
			fileError, static_cast<int>(pid), tid, attempt.empty() ? "none" : attempt.c_str());
	return length < 0 ? 0 : length;
}
} // namespace

void configureAndroidDiagnostics(const char* path)
{
	int openError = 0;
	bool newFile = false;
	{
		std::lock_guard<std::mutex> lock(gLogMutex);
		if (path && *path && gLogFd >= 0 && gLogPath == path) return;
		if (gLogFd >= 0) {
			::close(gLogFd);
			gLogFd = -1;
		}
		gLogPath = path ? path : "";
		if (!path || !*path) return;
		gLogFd = ::open(path, O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0600);
		if (gLogFd < 0) openError = errno;
		else {
			struct stat metadata{};
			newFile = fstat(gLogFd, &metadata) == 0 && metadata.st_size == 0;
		}
	}
	if (openError) {
		__android_log_print(ANDROID_LOG_ERROR, "BattleRealmsNative",
			"Could not open runtime diagnostics log (errno=%d)", openError);
		return;
	}
	if (newFile) logEventV(ANDROID_LOG_INFO, "BattleRealmsNative", "DIAGNOSTICS_FILE_CREATED",
		"JSONL session log opened in append mode", {});
}

void setAndroidDiagnosticsAttemptId(const char* attemptId)
{
	std::lock_guard<std::mutex> lock(gLogMutex);
	gAttemptId = attemptId ? attemptId : "";
}

int androidDiagnosticLogEvent(int priority, const char* tag, const char* event,
	const char* format, ...)
{
	va_list args;
	va_start(args, format);
	const int result = logEventV(priority, tag, event, format, args);
	va_end(args);
	return result;
}

int androidDiagnosticLogPrint(int priority, const char* tag, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	const int result = logEventV(priority, tag, "native_log", format, args);
	va_end(args);
	return result;
}

} // namespace br::runtime
