#ifndef _TASKPP_SHARED_STACK_H_
#define _TASKPP_SHARED_STACK_H_

#include <taskpp/task_stack.h>

namespace taskpp
{

class shared_stack
{
public:
	typedef boost::context::stack_traits traits_type;

	static uint32_t default_size() 
	{
		return 4096;
	}

	explicit shared_stack(const stack_param& param)
		: param_(param)
	{
		assert(param.init_size<=param.capacity);
		void* stack_data=malloc(param.capacity);
		shared_stack_.sp= ptradd(stack_data, param.capacity);
		shared_stack_.size=param.capacity;
	}
	~shared_stack()
	{
		free(ptrsub(shared_stack_.sp, shared_stack_.size));
	}

	stack_context allocate()
	{
		stack_context sctx;
		create_private_stack(sctx, param_.init_size);
		sctx.task_data=ptrsub(sctx.sp, param_.reservation);

		private_stack_context* priv_data=static_cast<private_stack_context*>(sctx.task_data)-1;
		priv_data->sctx_.sp=priv_data;
		priv_data->sctx_.size=sctx.size-param_.reservation-sizeof(private_stack_context);

		sctx.sp=shared_stack_.sp;
		sctx.size=shared_stack_.size;

		return sctx;
	}

	void deallocate( stack_context & sctx) const
	{
		private_stack_context* priv_sctx=private_stack(sctx);
		free_private_stack(priv_sctx->sctx_);
	}

	void before_resume_task(const stack_context& sctx, void* sp)
	{
		private_stack_context* priv_sctx=private_stack(sctx);
		ptrdiff_t count= ptrdiff(sp, shared_stack_.sp);
		copy_stack(shared_stack_, priv_sctx->sctx_, count);
	}

	void after_suspend_task(stack_context& sctx, void* sp)
	{
		private_stack_context* priv_sctx=private_stack(sctx);
		ptrdiff_t count= ptrdiff(sp, shared_stack_.sp);
		if(priv_sctx->sctx_.size<count)
		{
			stack_context new_sctx;
			size_t new_size=(count-1+param_.init_size)/param_.init_size*param_.init_size;
			create_private_stack(new_sctx, (uint32_t)new_size);
			if( !ptrequal(ptradd(priv_sctx->sctx_.sp, sizeof(private_stack_context)), sctx.task_data))
				free_private_stack(priv_sctx->sctx_);
			priv_sctx->sctx_=new_sctx;
		}
		copy_stack(priv_sctx->sctx_, shared_stack_, count);
	}

	void reset_stack(stack_context& sctx) const
	{
		private_stack_context* priv_sctx=private_stack(sctx);
		if( !ptrequal(ptradd(priv_sctx->sctx_.sp, sizeof(private_stack_context)), sctx.task_data))
		{
			free_private_stack(priv_sctx->sctx_);
			priv_sctx->sctx_.sp=priv_sctx;
			priv_sctx->sctx_.size=param_.init_size-param_.reservation-sizeof(private_stack_context);
		}
	}

private:

#pragma pack(push, stack_align)
	struct private_stack_context
	{
		boost::context::stack_context sctx_;
	};
#pragma pack(pop, stack_align)

	stack_param param_;
	boost::context::stack_context shared_stack_;

	static void create_private_stack(boost::context::stack_context& sctx, uint32_t size)
	{
		void* vp=malloc(size);
		if(vp==NULL) throw std::bad_alloc();

		sctx.sp=ptradd(vp, size);
		sctx.size=size;

#if defined(BOOST_USE_VALGRIND)
		sctx.valgrind_stack_id = VALGRIND_STACK_REGISTER(vp+size, vp);
#endif
	}

	static void free_private_stack(boost::context::stack_context& sctx)
	{
#if defined(BOOST_USE_VALGRIND)
		VALGRIND_STACK_DEREGISTER( priv_sctx->valgrind_stack_id);
#endif
		free(ptrsub(sctx.sp, sctx.size));
	}

	static private_stack_context* private_stack(const stack_context& sctx)
	{
		return  static_cast<private_stack_context*>(sctx.task_data)-1;
	}
};

}

#endif //_TASKPP_SHARED_STACK_H_
