#ifndef _TASKPP_TASK_CONTEXT_H_
#define _TASKPP_TASK_CONTEXT_H_

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif 

#include <taskpp/task_stack.h>

#if BOOST_VERSION<=106000
#include <boost/context/fcontext.hpp>
#else
#include <boost/context/detail/fcontext.hpp>
#endif

namespace taskpp
{

namespace detail
{

#if BOOST_VERSION<=106000
	using namespace boost::context;
#else
	using namespace boost::context::detail;
#endif

struct base_context
{
	fcontext_t sink_;
	fcontext_t mine_;

	base_context(void* stack, size_t size)
	{
		sink_=nullptr;
		mine_=make_fcontext(stack, size, &base_context::routine);
	}
	virtual ~base_context() { }

	bool resume();
	void yield();

#if BOOST_VERSION<=106000
	static void routine(intptr_t param);
#else
	static void routine(transfer_t sink);
#endif

private:
	static void coroutine_start(base_context* ctx);
	virtual void routine_implement()=0;
};


#ifdef _WIN32
inline void prefetch(void* p)
{
	PreFetchCacheLine(PF_TEMPORAL_LEVEL_1, p);
}

#else
inline void prefetch(void* p)
{
	__builtin_prefetch(p);
}

#endif //_WIN32

inline bool base_context::resume()
{
	bool ret=false;
	prefetch(mine_);

#if BOOST_VERSION<=106000
	ret=jump_fcontext(&sink_, mine_, reinterpret_cast<intptr_t>(this), false)!=0;
#else
	transfer_t transfer=jump_fcontext(mine_, this);
	mine_=transfer.fctx;
	ret= transfer.data!=nullptr;
#endif //BOOST_VERSION
	return ret;
}

inline void base_context::yield()
{
	prefetch(sink_);

#if BOOST_VERSION<=106000
	jump_fcontext(&mine_, sink_, reinterpret_cast<intptr_t>(this), false);
#else
	sink_=jump_fcontext(sink_, this).fctx;
#endif //BOOST_VERSION
}

#if BOOST_VERSION<=106000
inline void base_context::routine(intptr_t param)
{
	base_context* _this = reinterpret_cast<base_context*>(param);
	coroutine_start(_this);
	jump_fcontext(&_this->mine_, _this->sink_, NULL, false);
}
#else
inline void base_context::routine(transfer_t sink)
{
	base_context* _this = reinterpret_cast<base_context*>(sink.data);
	_this->sink_=sink.fctx;
	coroutine_start(_this);
	jump_fcontext(_this->sink_, NULL);
}
#endif //BOOST_VERSION

#if defined(_WIN32) && defined(_M_IX86)

/*
	In Windows X86 system, if an exception occurred, boost.context will cause _except_handler4 to crash
	use _SEH_prolog4 initialize SEH chain again
 */
extern "C"
void CoroutineStart(void (base_context::*routine)(), base_context* ctx);

inline void base_context::coroutine_start(base_context* ctx)
{
	CoroutineStart(&base_context::routine_implement, ctx);
}

#else

inline void base_context::coroutine_start(base_context* ctx)
{
	ctx->routine_implement();
}

#endif //_WIN32


}

}


#endif //_TASKPP_TASK_CONTEXT_H_
