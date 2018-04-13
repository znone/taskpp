#ifndef _TASKPP_TASK_STACK_H_
#define _TASKPP_TASK_STACK_H_

#include <boost/context/fixedsize_stack.hpp>
#include <stdint.h>

namespace taskpp
{

#if defined(_M_X64) || defined(__x86_64__)
	enum { stack_align = 16 };
#else
	enum { stack_align = sizeof(intptr_t) };
#endif

struct stack_context : public boost::context::stack_context 
{
	void* task_data;

	stack_context() : task_data(nullptr) { }
	stack_context(const boost::context::stack_context& src) 
		: boost::context::stack_context(src), task_data(nullptr)
	{
	}
};

struct stack_param
{
	uint32_t init_size;
	uint32_t capacity;
	uint32_t reservation;

	stack_param(): init_size((uint32_t)boost::context::stack_traits::default_size()), 
		capacity(0), reservation(0)
	{
	}
};

inline void* ptradd(void* p, ptrdiff_t offset)
{
	return static_cast<char*>(p)+offset;
}

inline void* ptrsub(void* p, ptrdiff_t offset)
{
	return static_cast<char*>(p)-offset;
}

inline ptrdiff_t ptrdiff(void* from, void* to)
{
	return reinterpret_cast<intptr_t>(to)-reinterpret_cast<intptr_t>(from);
}

inline void copy_stack(boost::context::stack_context& dest, const boost::context::stack_context& src, ptrdiff_t size)
{
	memcpy(static_cast<char*>(dest.sp)-size, static_cast<char*>(src.sp)-size, size);
}

inline bool ptrequal(void* p1, void* p2)
{
	return reinterpret_cast<intptr_t>(p1)==reinterpret_cast<intptr_t>(p2);
}

class empty_task
{
public:
	explicit empty_task(const stack_param&) { }

	static uint32_t default_size() { return 0; }

	stack_context allocate() { return stack_context(); }
	void before_resume_task(stack_context& sctx, void* sp) const { }
	void after_suspend_task(stack_context& sctx, void* sp) const { }
	void reset_stack(stack_context& sctx) const { }


};

}

#endif //_TASKPP_TASK_STACK_H_
