#include "stdafx.h"
#include "LoadingHost.h"

namespace
{
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
	  m_nestingDepth(0)
{
	if (m_enabled)
		Msg("* [loading-host] enabled; static provider active");
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

	if (--m_nestingDepth == 0 && m_state != ELoadingHostState::Error)
		SetState(ELoadingHostState::Finalizing);
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

	m_presenting = true;
	SLoadingHostFrame frame = {m_state, m_sessionId, m_nestingDepth};
	presenter.DrawLoadingHost(frame);
	m_presenting = false;
	Device.EndLoadingFrame();
	return true;
}
