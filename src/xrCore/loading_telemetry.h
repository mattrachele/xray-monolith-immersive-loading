#pragma once

#include "xrCore.h"

struct XRCORE_API LoadingTelemetryToken
{
	u64 span_id;
	u64 session_id;
	u64 start_ticks;
	u64 parent_span_id;
	LPCSTR task_id;
	LPCSTR parent_task_id;
	u32 thread_id;
	bool active;

	LoadingTelemetryToken();
};

class XRCORE_API LoadingTelemetry
{
public:
	static bool Enabled();
	static bool DetailedEnabled();
	static void InitializeOutput();
	static void Shutdown();

	static LoadingTelemetryToken BeginSession(LPCSTR session_kind);
	static void EndSession(LoadingTelemetryToken& token);
	static void EndActiveSession();
	static bool HasActiveSession();

	static LoadingTelemetryToken BeginSpan(LPCSTR task_id);
	static void EndSpan(LoadingTelemetryToken& token, u64 units = 0, u64 bytes = 0);
	static void Instant(LPCSTR task_id, LPCSTR value = nullptr);

	static void MarkFirstLoadingFrame();
	static void MarkFirstDestinationFrame();
	static void MarkPlayerInputReady();
	static void RecordLoadingFrame();
	static void RecordProgress(u64 completed, u64 total);
};

class XRCORE_API LoadingTelemetryScope
{
public:
	explicit LoadingTelemetryScope(LPCSTR task_id);
	~LoadingTelemetryScope();

	void SetUnits(u64 units);
	void SetBytes(u64 bytes);

private:
	LoadingTelemetryToken m_token;
	u64 m_units;
	u64 m_bytes;
};

#define LOADING_TELEMETRY_JOIN_IMPL(a, b) a##b
#define LOADING_TELEMETRY_JOIN(a, b) LOADING_TELEMETRY_JOIN_IMPL(a, b)
#define LOADING_TELEMETRY_SCOPE(task_id) \
	LoadingTelemetryScope LOADING_TELEMETRY_JOIN(loadingTelemetryScope_, __LINE__)(task_id)
