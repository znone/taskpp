#ifndef _TASK_FIXED_STACK_H_
#define _TASK_FIXED_STACK_H_

#include "task_stack.h"

namespace task
{

class fixedsize_stack : public boost::context::fixedsize_stack
{
public:

	explicit fixedsize_stack(const stack_param& param) : 
		boost::context::fixedsize_stack(param.init_size),
		reservation_(param.reservation) 
	{
	}

	static uint32_t default_size() 
	{
		return traits_type::default_size();
	}

	stack_context allocate()
	{
		stack_context sctx;
		sctx=boost::context::fixedsize_stack::allocate();
		if(reservation_>0)
		{
			sctx.sp=ptrsub(sctx.sp, reservation_);
			sctx.size-=reservation_;
			sctx.task_data=sctx.sp;
		}
		return sctx;
	}

	void before_resume_task(stack_context& sctx, void* sp) const { }
	void after_suspend_task(stack_context& sctx, void* sp) const { }
	void reset_stack(stack_context& sctx) const { }

private:
	uint32_t reservation_;

};

}

#endif //_TASK_FIXED_STACK_H_
