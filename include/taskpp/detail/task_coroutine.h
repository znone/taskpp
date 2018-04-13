#ifndef _TASKPP_TASK_COROUTINE_H_
#define _TASKPP_TASK_COROUTINE_H_

#include <taskpp/detail/task_context.h>

namespace taskpp
{

class base_task;

namespace detail
{

struct coroutine_context : public base_context
{
	stack_context stack_;
	base_task* task_;

	coroutine_context(base_task* task, stack_context& stack) 
		: base_context(stack.sp, stack.size), stack_(stack), task_(task)
	{
	}

	static coroutine_context* create_coroutine(base_task* task, stack_context& stack)
	{
		coroutine_context* coroutine=static_cast<coroutine_context*>(stack.task_data);
		new(coroutine) coroutine_context(task, stack);
		return coroutine;
	}

	bool resume()
	{
		bool ret=base_context::resume();
		check_stack_overflow();
		return ret;
	}

	size_t stack_size() const { return stack_.size; }

	size_t remaining_stack() const
	{
		return (intptr_t)mine_ - ((intptr_t)stack_.sp-stack_.size);
	}

private:
	virtual void routine_implement() override;

	void check_stack_overflow() const
	{
		if( (intptr_t)mine_ > (intptr_t)stack_.sp ||
			(intptr_t)mine_ < ((intptr_t)stack_.sp-stack_.size)) 
			abort();
	}
};

class base_work_thread;

}

}


#endif //_TASKPP_TASK_COROUTINE_H_
