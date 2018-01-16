#ifndef _TASKPP_CALL_IN_NEW_STACK_H_
#define _TASKPP_CALL_IN_NEW_STACK_H_

#include <type_traits>
#include <exception>
#include <taskpp/detail/task_context.h>
#include <boost/context/stack_context.hpp>

namespace taskpp
{

namespace detail
{

class function_context : public base_context
{
public:
	function_context(void* stack, size_t stack_size, boost::context::stack_context* cur_stcx)
		: base_context(stack, stack_size)
	{
		cur_stcx_=cur_stcx;
		if(cur_stcx)
		{
			old_stcx_=*cur_stcx;
			cur_stcx->sp=stack;
			cur_stcx->size=stack_size;
		}
	}

private:
	boost::context::stack_context* cur_stcx_;
	boost::context::stack_context old_stcx_;

protected:
	virtual void routine_implement() override
	{
		if(cur_stcx_) *cur_stcx_=old_stcx_;
	}
};

template<typename R>
class call_in_new_stack_helper : public function_context
{
public:
	typedef R result_type;
	template<typename F, typename... Args>
	explicit call_in_new_stack_helper(void* stack, size_t stack_size, boost::context::stack_context* cur_stcx,
		F&& fun, Args&&... args)
		: function_context(stack, stack_size, cur_stcx),
		function_(std::bind(std::forward<F>(fun), std::forward<Args>(args)...))
	{
		resume();
	}

	const R& result() const
	{
		if(exception_) 
			std::rethrow_exception(exception_);
		return result_; 
	}

private:
	result_type result_;
	std::function<result_type()> function_;
	std::exception_ptr exception_;

	virtual void routine_implement() override
	{
		try
		{
			result_=function_();
		}
		catch (...)
		{
			exception_=std::current_exception();
		}
		function_context::routine_implement();
	}
};

template<>
class call_in_new_stack_helper<void> : public function_context
{
public:
	typedef void result_type;
	template<typename F, typename... Args>
	explicit call_in_new_stack_helper(void* stack, size_t stack_size, boost::context::stack_context* cur_stcx,
		F&& fun, Args&&... args)
		: function_context(stack, stack_size, cur_stcx),
		function_(std::bind(std::forward<F>(fun), std::forward<Args>(args)...))
	{
		resume();
	}

	void result() const
	{
		if(exception_) 
			std::rethrow_exception(exception_);
	}

private:
	std::function<result_type()> function_;
	std::exception_ptr exception_;

	virtual void routine_implement() override
	{
		try
		{
			function_();
		}
		catch (...)
		{
			exception_=std::current_exception();
		}
		function_context::routine_implement();
	}
};

}

template<typename F, typename... Args>
inline auto call_in_new_stack(void* stack, size_t stack_size, F&& fun, Args&&... args)
	-> decltype(fun(args...))
{
	typedef decltype(fun(args...)) result_type;
	base_task* task=this_task::self();
	detail::call_in_new_stack_helper<result_type> helper(reinterpret_cast<char*>(stack)+stack_size, stack_size,
		task ? &task->coroutine()->stack_ : nullptr,
		std::forward<F>(fun), std::forward<Args>(args)...);
	return helper.result();
}

template<typename F, typename... Args>
inline auto call_in_new_stack(size_t stack_size, F&& fun, Args&&... args)
	-> decltype(fun(args...))
{
	std::vector<char> stack(stack_size);
	return call_in_new_stack(stack.data(), stack.size(), 
		std::forward<F>(fun), std::forward<Args>(args)...);
}

}

#endif //_TASKPP_CALL_IN_NEW_STACK_H_
