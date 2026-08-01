#pragma once

enum class ELoadingHostState : u8
{
	Disabled,
	Idle,
	Starting,
	Loading,
	Finalizing,
	Error,
	Handoff,
	Shutdown,
};

struct SLoadingHostFrame
{
	ELoadingHostState state;
	u32 session_id;
	u32 nesting_depth;
};

class ENGINE_API ILoadingHostPresenter
{
public:
	virtual ~ILoadingHostPresenter() = default;
	virtual void DrawLoadingHost(const SLoadingHostFrame& frame) = 0;
};

class ENGINE_API CLoadingHost
{
public:
	explicit CLoadingHost(bool enabled);
	~CLoadingHost();

	bool Enabled() const { return m_enabled; }
	ELoadingHostState State() const { return m_state; }
	u32 SessionId() const { return m_sessionId; }
	u32 NestingDepth() const { return m_nestingDepth; }
	u32 FrameBudgetMs() const { return m_frameBudgetMs; }

	void Begin();
	void End();
	void Fail();
	void CompleteHandoff();
	void Shutdown();

	bool YieldIfDue(ILoadingHostPresenter& presenter);
	bool DrawInActiveFrame(ILoadingHostPresenter& presenter);

private:
	bool IsPrimaryThread() const;
	bool Present(ILoadingHostPresenter& presenter);
	void DrawProvider(ILoadingHostPresenter& presenter);
	void SetState(ELoadingHostState state);

private:
	bool m_enabled;
	bool m_presenting;
	ELoadingHostState m_state;
	u32 m_primaryThreadId;
	u32 m_sessionId;
	u32 m_nestingDepth;
	u32 m_frameBudgetMs;
	u32 m_lastYieldMs;
	u32 m_yieldChecks;
	u32 m_yieldsPresented;
	u32 m_yieldsSkipped;
	bool m_hasYielded;
	u32 m_lastPresentedFrame;
	u32 m_blockingOwnerSession;
	u32 m_activeFrameOwnerSession;
};
