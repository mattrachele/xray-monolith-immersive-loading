#include "stdafx.h"
#include "LoadingHost.h"

namespace
{
constexpr u32 kDefaultLoadingFrameBudgetMs = 33;
constexpr u32 kMinimumLoadingFrameBudgetMs = 16;
constexpr u32 kMaximumLoadingFrameBudgetMs = 33;

u32 loading_frame_budget_ms()
{
	if (!Core.Params)
		return kDefaultLoadingFrameBudgetMs;

	constexpr LPCSTR marker = "-immersive_loading_budget_ms ";
	LPCSTR value = strstr(Core.Params, marker);
	if (!value)
		return kDefaultLoadingFrameBudgetMs;

	value += xr_strlen(marker);
	LPSTR end = nullptr;
	const long parsed = strtol(value, &end, 10);
	if (end == value || (*end != '\0' && *end != ' ') || parsed < kMinimumLoadingFrameBudgetMs ||
	    parsed > kMaximumLoadingFrameBudgetMs)
	{
		Msg("! [loading-host] invalid frame budget; expected %u-%u ms, using %u ms",
		    kMinimumLoadingFrameBudgetMs, kMaximumLoadingFrameBudgetMs, kDefaultLoadingFrameBudgetMs);
		return kDefaultLoadingFrameBudgetMs;
	}

	return static_cast<u32>(parsed);
}

LPCSTR loading_host_state_name(ELoadingHostState state)
{
	switch (state)
	{
	case ELoadingHostState::Disabled: return "disabled";
	case ELoadingHostState::Idle: return "idle";
	case ELoadingHostState::Starting: return "starting";
	case ELoadingHostState::Loading: return "loading";
	case ELoadingHostState::Finalizing: return "finalizing";
	case ELoadingHostState::Error: return "error";
	case ELoadingHostState::Handoff: return "handoff";
	case ELoadingHostState::Shutdown: return "shutdown";
	}

	NODEFAULT;
	return "unknown";
}
}

CLoadingHost::CLoadingHost(bool enabled)
	: m_enabled(enabled),
	  m_presenting(false),
	  m_state(enabled ? ELoadingHostState::Idle : ELoadingHostState::Disabled),
	  m_primaryThreadId(GetCurrentThreadId()),
	  m_sessionId(0),
	  m_nestingDepth(0),
	  m_frameBudgetMs(loading_frame_budget_ms()),
	  m_lastYieldMs(0),
	  m_yieldChecks(0),
	  m_yieldsPresented(0),
	  m_yieldsSkipped(0),
	  m_hasYielded(false),
	  m_lastPresentedFrame(u32(-1)),
	  m_blockingOwnerSession(u32(-1)),
	  m_activeFrameOwnerSession(u32(-1))
{
	if (m_enabled)
		Msg("* [loading-host] enabled; static provider active; frame_budget_ms=%u", m_frameBudgetMs);
}

CLoadingHost::~CLoadingHost()
{
	Shutdown();
}

bool CLoadingHost::IsPrimaryThread() const
{
	return GetCurrentThreadId() == m_primaryThreadId;
}

void CLoadingHost::SetState(ELoadingHostState state)
{
	if (m_state == state)
		return;

	m_state = state;
	Msg("* [loading-host] session=%u depth=%u state=%s", m_sessionId, m_nestingDepth,
	    loading_host_state_name(state));
}

void CLoadingHost::Begin()
{
	if (!m_enabled)
		return;

	VERIFY(IsPrimaryThread());
	VERIFY(m_state != ELoadingHostState::Shutdown);

	if (m_nestingDepth++ == 0)
	{
		++m_sessionId;
		m_lastYieldMs = 0;
		m_yieldChecks = 0;
		m_yieldsPresented = 0;
		m_yieldsSkipped = 0;
		m_hasYielded = false;
		SetState(ELoadingHostState::Starting);
		SetState(ELoadingHostState::Loading);
	}
}

void CLoadingHost::End()
{
	if (!m_enabled)
		return;

	VERIFY(IsPrimaryThread());
	VERIFY(m_nestingDepth > 0);

	if (--m_nestingDepth == 0)
	{
		Msg("* [loading-host] session=%u budget_ms=%u yield_checks=%u presented=%u skipped=%u",
		    m_sessionId, m_frameBudgetMs, m_yieldChecks, m_yieldsPresented, m_yieldsSkipped);
		if (m_state != ELoadingHostState::Error)
			SetState(ELoadingHostState::Finalizing);
	}
}

void CLoadingHost::Fail()
{
	if (!m_enabled)
		return;

	VERIFY(IsPrimaryThread());
	SetState(ELoadingHostState::Error);
}

void CLoadingHost::CompleteHandoff()
{
	if (!m_enabled || m_state == ELoadingHostState::Idle || m_state == ELoadingHostState::Disabled ||
	    m_state == ELoadingHostState::Shutdown)
	{
		return;
	}

	VERIFY(IsPrimaryThread());
	VERIFY(m_nestingDepth == 0);
	SetState(ELoadingHostState::Handoff);
}

void CLoadingHost::Shutdown()
{
	if (!m_enabled || m_state == ELoadingHostState::Shutdown)
		return;

	VERIFY(IsPrimaryThread());
	VERIFY(!m_presenting);
	m_nestingDepth = 0;
	SetState(ELoadingHostState::Shutdown);
}

bool CLoadingHost::Present(ILoadingHostPresenter& presenter)
{
	if (!m_enabled || m_presenting)
		return false;

	VERIFY(IsPrimaryThread());
	VERIFY(m_state == ELoadingHostState::Loading || m_state == ELoadingHostState::Finalizing ||
	       m_state == ELoadingHostState::Error);

	if (!Device.BeginLoadingFrame())
		return false;

	if (m_blockingOwnerSession != m_sessionId)
	{
		m_blockingOwnerSession = m_sessionId;
		Msg("* [loading-host] session=%u owner=host phase=blocking provider=static", m_sessionId);
	}

	DrawProvider(presenter);
	Device.EndLoadingFrame();
	return true;
}

bool CLoadingHost::YieldIfDue(ILoadingHostPresenter& presenter)
{
	if (!m_enabled || m_presenting)
		return false;

	VERIFY(IsPrimaryThread());
	VERIFY(m_state == ELoadingHostState::Loading || m_state == ELoadingHostState::Error);

	++m_yieldChecks;
	const u32 now = Device.TimerAsync();
	if (m_hasYielded && now - m_lastYieldMs < m_frameBudgetMs)
	{
		++m_yieldsSkipped;
		return false;
	}

	Device.dwFrame += 1;
	if (!Present(presenter))
		return false;

	m_lastYieldMs = Device.TimerAsync();
	m_hasYielded = true;
	++m_yieldsPresented;
	return true;
}

bool CLoadingHost::DrawInActiveFrame(ILoadingHostPresenter& presenter)
{
	if (!m_enabled || m_presenting || m_state == ELoadingHostState::Shutdown)
		return false;

	VERIFY(IsPrimaryThread());

	if (m_activeFrameOwnerSession != m_sessionId)
	{
		m_activeFrameOwnerSession = m_sessionId;
		Msg("* [loading-host] session=%u owner=host phase=active-frame provider=static", m_sessionId);
	}

	DrawProvider(presenter);
	return true;
}

void CLoadingHost::DrawProvider(ILoadingHostPresenter& presenter)
{
	VERIFY(IsPrimaryThread());
	VERIFY(!m_presenting);
	VERIFY(m_lastPresentedFrame != Device.dwFrame);

	m_presenting = true;
	SLoadingHostFrame frame = {m_state, m_sessionId, m_nestingDepth};
	presenter.DrawLoadingHost(frame);
	m_presenting = false;
	m_lastPresentedFrame = Device.dwFrame;
}
