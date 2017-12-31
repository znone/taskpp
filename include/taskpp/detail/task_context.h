#ifndef _TASK_CONTEXT_H_
#define _TASK_CONTEXT_H_

#if BOOST_VERSION<=106000
#include <boost/context/fcontext.hpp>
#else
#include <boost/context/detail/fcontext.hpp>
#endif
#include <taskpp/task_stack.h>

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

	bool resume()
	{
		bool ret=false;
#if BOOST_VERSION<=106000
		ret=jump_fcontext(&sink_, mine_, reinterpret_cast<intptr_t>(this), false)!=0;
#else
		transfer_t transfer=jump_fcontext(mine_, this);
		mine_=transfer.fctx;
		ret= transfer.data!=nullptr;
#endif //BOOST_VERSION
		return ret;
	}

	void yield()
	{
#if BOOST_VERSION<=106000
		jump_fcontext(&mine_, sink_, reinterpret_cast<intptr_t>(this), false);
#else
		sink_=jump_fcontext(sink_, this).fctx;
#endif //BOOST_VERSION
	}

#if BOOST_VERSION<=106000
	static void routine(intptr_t param)
	{
		base_context* _this = reinterpret_cast<base_context*>(param);
		_this->routine_implement();
		jump_fcontext(&_this->mine_, _this->sink_, NULL, false);
	}
#else
	static void routine(transfer_t sink)
	{
		base_context* _this = reinterpret_cast<base_context*>(sink.data);
		_this->sink_=sink.fctx;
		_this->routine_implement();
		jump_fcontext(_this->sink_, NULL);
	}
#endif //BOOST_VERSION

private:
	virtual void routine_implement()=0;
};

}


}


#endif //_TASK_CONTEXT_H_
