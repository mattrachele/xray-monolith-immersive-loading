#include "stdafx.h"
#include "loading_telemetry.h"
#include "loading_telemetry_build.h"

#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>

extern XRCORE_API u32 build_id;
extern XRCORE_API LPCSTR build_date;

namespace
{
	constexpr u32 kSchemaVersion = 1;
	constexpr u32 kMaxDepth = 64;

	struct ActiveSpan
	{
		u64 span_id;
		LPCSTR task_id;
	};

	struct CompletedSpan
	{
		u64 span_id;
		u64 session_id;
		u64 start_ticks;
		u64 elapsed_ticks;
		u64 parent_span_id;
		LPCSTR task_id;
		LPCSTR parent_task_id;
		u32 thread_id;
		u64 units;
		u64 bytes;
	};

	thread_local ActiveSpan g_stack[kMaxDepth];
	thread_local u32 g_depth = 0;

	std::atomic<u64> g_next_span_id(1);
	std::atomic<u64> g_next_session_id(1);
	std::mutex g_output_mutex;
	std::mutex g_completed_span_mutex;
	std::ofstream g_output;
	xr_vector<xr_string> g_pending_lines;
	xr_vector<CompletedSpan> g_completed_spans;
	LARGE_INTEGER g_frequency = {};
	LARGE_INTEGER g_origin = {};
	std::atomic<u64> g_active_session(0);
	u64 g_session_start_ticks = 0;
	LoadingTelemetryToken g_session_token;
	LoadingTelemetryToken g_post_load_wait_token;
	bool g_first_loading_frame = false;
	bool g_first_destination_frame = false;
	bool g_player_input_ready = false;
	u64 g_last_loading_frame_ticks = 0;

	u64 ticks_now()
	{
		LARGE_INTEGER value;
		QueryPerformanceCounter(&value);
		return static_cast<u64>(value.QuadPart);
	}

	bool has_command_option(LPCSTR name)
	{
		if (!Core.Params || !name)
			return false;

		xr_string marker = "-";
		marker += name;
		const size_t marker_length = marker.size();
		LPCSTR cursor = Core.Params;
		while ((cursor = strstr(cursor, marker.c_str())) != nullptr)
		{
			const bool begins_token = cursor == Core.Params || cursor[-1] == ' ';
			const char suffix = cursor[marker_length];
			const bool ends_token = suffix == '\0' || suffix == ' ';
			if (begins_token && ends_token)
				return true;
			cursor += marker_length;
		}
		return false;
	}

	double ticks_to_ms(u64 ticks)
	{
		if (!g_frequency.QuadPart)
			QueryPerformanceFrequency(&g_frequency);
		return (static_cast<double>(ticks) * 1000.0) / static_cast<double>(g_frequency.QuadPart);
	}

	xr_string json_escape(LPCSTR value)
	{
		xr_string result;
		if (!value)
			return result;

		for (const unsigned char* cursor = reinterpret_cast<const unsigned char*>(value); *cursor; ++cursor)
		{
			switch (*cursor)
			{
			case '\\': result += "\\\\"; break;
			case '"': result += "\\\""; break;
			case '\n': result += "\\n"; break;
			case '\r': result += "\\r"; break;
			case '\t': result += "\\t"; break;
			default:
				if (*cursor < 0x20)
				result += "?";
				else
					result += static_cast<char>(*cursor);
			}
		}
		return result;
	}

	xr_string command_value(LPCSTR name)
	{
		if (!Core.Params || !name)
			return "";

		xr_string marker = "-";
		marker += name;
		marker += " ";
		LPCSTR start = strstr(Core.Params, marker.c_str());
		if (!start)
			return "";
		start += marker.size();
		LPCSTR end = strchr(start, ' ');
		return end ? xr_string(start, end - start) : xr_string(start);
	}

	void emit_line(const xr_string& line)
	{
		std::lock_guard<std::mutex> guard(g_output_mutex);
		if (g_output.is_open())
		{
			g_output << line.c_str() << '\n';
		}
		else
		{
			g_pending_lines.push_back(line);
		}
	}

	void emit_instant(LPCSTR task_id, LPCSTR value, u64 timestamp_ticks)
	{
		std::ostringstream json;
		json.setf(std::ios::fixed);
		json.precision(3);
		json << "{\"schema_version\":" << kSchemaVersion
			<< ",\"record\":\"instant\""
			<< ",\"session_id\":" << g_active_session.load()
			<< ",\"task_id\":\"" << json_escape(task_id).c_str() << "\""
			<< ",\"timestamp_ms\":" << ticks_to_ms(timestamp_ticks - static_cast<u64>(g_origin.QuadPart))
			<< ",\"thread_id\":" << GetCurrentThreadId();
		if (value)
			json << ",\"value\":\"" << json_escape(value).c_str() << "\"";
		json << "}";
		emit_line(json.str().c_str());
	}

	void flush_completed_spans()
	{
		xr_vector<CompletedSpan> completed;
		{
			std::lock_guard<std::mutex> guard(g_completed_span_mutex);
			completed.swap(g_completed_spans);
		}
		if (completed.empty())
			return;

		std::ostringstream batch;
		batch.setf(std::ios::fixed);
		batch.precision(3);
		for (const CompletedSpan& span : completed)
		{
			batch << "{\"schema_version\":" << kSchemaVersion
				<< ",\"record\":\"span\""
				<< ",\"session_id\":" << span.session_id
				<< ",\"span_id\":" << span.span_id
				<< ",\"task_id\":\"" << json_escape(span.task_id).c_str() << "\"";
			if (span.parent_task_id)
				batch << ",\"parent_span_id\":" << span.parent_span_id
					<< ",\"parent_task_id\":\"" << json_escape(span.parent_task_id).c_str() << "\"";
			else
				batch << ",\"parent_span_id\":null,\"parent_task_id\":null";
			batch << ",\"start_ms\":" << ticks_to_ms(span.start_ticks - static_cast<u64>(g_origin.QuadPart))
				<< ",\"elapsed_ms\":" << ticks_to_ms(span.elapsed_ticks)
				<< ",\"thread_id\":" << span.thread_id
				<< ",\"units\":" << span.units
				<< ",\"bytes\":" << span.bytes
				<< "}\n";
		}

		const std::string lines = batch.str();
		std::lock_guard<std::mutex> guard(g_output_mutex);
		if (g_output.is_open())
			g_output << lines;
		else
			g_pending_lines.push_back(lines.c_str());
	}

	LoadingTelemetryToken begin_detached_session_span(LPCSTR task_id)
	{
		LoadingTelemetryToken token;
		if (!LoadingTelemetry::DetailedEnabled() || !g_session_token.active)
			return token;

		token.span_id = g_next_span_id.fetch_add(1);
		token.session_id = g_active_session.load();
		token.start_ticks = ticks_now();
		token.parent_span_id = g_session_token.span_id;
		token.task_id = task_id;
		token.parent_task_id = g_session_token.task_id;
		token.thread_id = GetCurrentThreadId();
		token.active = true;
		return token;
	}
}

LoadingTelemetryToken::LoadingTelemetryToken() :
	span_id(0),
	session_id(0),
	start_ticks(0),
	parent_span_id(0),
	task_id(nullptr),
	parent_task_id(nullptr),
	thread_id(0),
	active(false)
{
}

bool LoadingTelemetry::Enabled()
{
	return has_command_option("loading_telemetry") || has_command_option("loading_telemetry_control");
}

bool LoadingTelemetry::DetailedEnabled()
{
	return has_command_option("loading_telemetry");
}

void LoadingTelemetry::InitializeOutput()
{
	if (!Enabled())
		return;

	std::lock_guard<std::mutex> guard(g_output_mutex);
	if (g_output.is_open())
		return;

	if (!g_frequency.QuadPart)
		QueryPerformanceFrequency(&g_frequency);
	if (!g_origin.QuadPart)
		QueryPerformanceCounter(&g_origin);
	g_completed_spans.reserve(512);

	string_path path;
	FS.update_path(path, "$logs$", "loading_telemetry.jsonl");
	g_output.open(path, std::ios::out | std::ios::trunc);
	if (!g_output.is_open())
	{
		Msg("! [loading-telemetry] cannot open %s", path);
		return;
	}

	std::ostringstream metadata;
	metadata << "{\"schema_version\":" << kSchemaVersion
		<< ",\"record\":\"metadata\""
		<< ",\"engine_commit\":\"" << LOADING_TELEMETRY_ENGINE_COMMIT << "\""
		<< ",\"engine_build\":" << build_id
		<< ",\"engine_build_date\":\"" << json_escape(build_date).c_str() << "\""
		<< ",\"telemetry_mode\":\"" << (DetailedEnabled() ? "detailed" : "control") << "\""
		<< ",\"profile_fingerprint\":\"" << json_escape(command_value("loading_profile_fingerprint").c_str()).c_str() << "\""
		<< ",\"companion_commit\":\"" << json_escape(command_value("loading_companion_commit").c_str()).c_str() << "\""
		<< ",\"command_line\":\"" << json_escape(Core.Params).c_str() << "\""
		<< "}";
	g_output << metadata.str() << '\n';
	for (const xr_string& line : g_pending_lines)
		g_output << line.c_str() << '\n';
	g_pending_lines.clear();
	g_output.flush();
	Msg("* [loading-telemetry] schema %u -> %s", kSchemaVersion, path);
}

void LoadingTelemetry::Shutdown()
{
	flush_completed_spans();
	std::lock_guard<std::mutex> guard(g_output_mutex);
	if (g_output.is_open())
	{
		g_output.flush();
		g_output.close();
	}
	g_pending_lines.clear();
}

LoadingTelemetryToken LoadingTelemetry::BeginSession(LPCSTR session_kind)
{
	if (!Enabled())
		return LoadingTelemetryToken();
	if (g_active_session.load())
		return g_session_token;

	g_active_session.store(g_next_session_id.fetch_add(1));
	g_session_start_ticks = ticks_now();
	g_first_loading_frame = false;
	g_first_destination_frame = false;
	g_player_input_ready = false;
	g_last_loading_frame_ticks = 0;
	LoadingTelemetryToken token = BeginSpan(session_kind);
	g_session_token = token;
	Instant("loading.session.begin", session_kind);
	Instant("loading.input_feedback", "unavailable_before_loading_host");
	return token;
}

void LoadingTelemetry::EndSession(LoadingTelemetryToken& token)
{
	if (!token.active)
		return;
	Instant("loading.session.end", token.task_id);
	EndSpan(token);
	// Span formatting and file writes happen after loading.total has stopped so
	// detailed telemetry does not extend the measured loading interval.
	flush_completed_spans();
	{
		std::lock_guard<std::mutex> guard(g_output_mutex);
		if (g_output.is_open())
			g_output.flush();
	}
	g_active_session.store(0);
	g_session_start_ticks = 0;
	g_session_token = LoadingTelemetryToken();
	g_post_load_wait_token = LoadingTelemetryToken();
}

void LoadingTelemetry::EndActiveSession()
{
	if (g_session_token.active)
		EndSession(g_session_token);
}

bool LoadingTelemetry::HasActiveSession()
{
	return g_active_session.load() != 0;
}

LoadingTelemetryToken LoadingTelemetry::BeginSpan(LPCSTR task_id)
{
	LoadingTelemetryToken token;
	if (!Enabled())
		return token;
	if (!DetailedEnabled() && xr_strcmp(task_id, "loading.total"))
		return token;

	if (!g_origin.QuadPart)
		QueryPerformanceCounter(&g_origin);

	token.span_id = g_next_span_id.fetch_add(1);
	token.session_id = g_active_session.load();
	token.start_ticks = ticks_now();
	token.task_id = task_id;
	token.parent_span_id = g_depth ? g_stack[g_depth - 1].span_id : 0;
	token.parent_task_id = g_depth ? g_stack[g_depth - 1].task_id : nullptr;
	token.thread_id = GetCurrentThreadId();
	token.active = true;

	if (g_depth < kMaxDepth)
	{
		g_stack[g_depth].span_id = token.span_id;
		g_stack[g_depth].task_id = task_id;
		++g_depth;
	}
	return token;
}

void LoadingTelemetry::EndSpan(LoadingTelemetryToken& token, u64 units, u64 bytes)
{
	if (!token.active)
		return;

	const u64 end_ticks = ticks_now();
	for (u32 index = g_depth; index > 0; --index)
	{
		if (g_stack[index - 1].span_id != token.span_id)
			continue;
		for (u32 move = index; move < g_depth; ++move)
			g_stack[move - 1] = g_stack[move];
		--g_depth;
		break;
	}

	CompletedSpan completed = {};
	completed.span_id = token.span_id;
	completed.session_id = token.session_id;
	completed.start_ticks = token.start_ticks;
	completed.elapsed_ticks = end_ticks - token.start_ticks;
	completed.parent_span_id = token.parent_span_id;
	completed.task_id = token.task_id;
	completed.parent_task_id = token.parent_task_id;
	completed.thread_id = token.thread_id;
	completed.units = units;
	completed.bytes = bytes;
	{
		std::lock_guard<std::mutex> guard(g_completed_span_mutex);
		g_completed_spans.push_back(completed);
	}
	token.active = false;
}

void LoadingTelemetry::Instant(LPCSTR task_id, LPCSTR value)
{
	if (Enabled() && (DetailedEnabled() || !strncmp(task_id, "loading.", 8)))
		emit_instant(task_id, value, ticks_now());
}

void LoadingTelemetry::MarkFirstLoadingFrame()
{
	if (!Enabled() || g_first_loading_frame)
		return;
	g_first_loading_frame = true;
	const u64 now = ticks_now();
	emit_instant("loading.first_responsive_frame", nullptr, now);

	LoadingTelemetryToken token;
	token.span_id = g_next_span_id.fetch_add(1);
	token.session_id = g_active_session;
	token.start_ticks = g_session_start_ticks;
	token.task_id = "loading.confirmation_to_first_frame";
	token.thread_id = GetCurrentThreadId();
	token.active = g_session_start_ticks != 0;
	EndSpan(token);
}

void LoadingTelemetry::MarkEngineLoadEnd()
{
	if (!Enabled())
		return;
	Instant("loading.engine_load_end");
	if (!g_post_load_wait_token.active)
		g_post_load_wait_token = begin_detached_session_span("activation.post_load_precache_wait");
}

void LoadingTelemetry::MarkFirstDestinationFrame()
{
	if (!Enabled() || g_first_destination_frame)
		return;
	g_first_destination_frame = true;
	Instant("loading.first_destination_frame");
}

void LoadingTelemetry::MarkPlayerInputReady()
{
	if (!Enabled() || g_player_input_ready)
		return;
	g_player_input_ready = true;
	EndSpan(g_post_load_wait_token);
	Instant("loading.player_input_ready");
}

void LoadingTelemetry::RecordLoadingFrame()
{
	if (!DetailedEnabled())
		return;
	const u64 now = ticks_now();
	if (g_last_loading_frame_ticks)
	{
		std::ostringstream json;
		json.setf(std::ios::fixed);
		json.precision(3);
		json << "{\"schema_version\":" << kSchemaVersion
			<< ",\"record\":\"loading_frame\""
			<< ",\"session_id\":" << g_active_session.load()
			<< ",\"timestamp_ms\":" << ticks_to_ms(now - static_cast<u64>(g_origin.QuadPart))
			<< ",\"interval_ms\":" << ticks_to_ms(now - g_last_loading_frame_ticks)
			<< "}";
		emit_line(json.str().c_str());
	}
	g_last_loading_frame_ticks = now;
}

void LoadingTelemetry::RecordProgress(u64 completed, u64 total)
{
	if (!DetailedEnabled())
		return;
	std::ostringstream json;
	json << "{\"schema_version\":" << kSchemaVersion
		<< ",\"record\":\"progress\""
		<< ",\"session_id\":" << g_active_session.load()
		<< ",\"completed\":" << completed
		<< ",\"total\":" << total
		<< "}";
	emit_line(json.str().c_str());
}

LoadingTelemetryScope::LoadingTelemetryScope(LPCSTR task_id) :
	m_token(LoadingTelemetry::BeginSpan(task_id)),
	m_units(0),
	m_bytes(0)
{
}

LoadingTelemetryScope::~LoadingTelemetryScope()
{
	LoadingTelemetry::EndSpan(m_token, m_units, m_bytes);
}

void LoadingTelemetryScope::SetUnits(u64 units)
{
	m_units = units;
}

void LoadingTelemetryScope::SetBytes(u64 bytes)
{
	m_bytes = bytes;
}
