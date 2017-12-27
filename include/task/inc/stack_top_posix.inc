
namespace task
{

namespace this_task 
{

inline void* stack_top()
{
	void* x;
#if defined(__i386__)
	__asm__("movl %%esp, %0" : "=X"(x));
#elif defined(__x86_64__)
	__asm__("movq %%rsp, %0" : "=X"(x));
#else
	#error "unsupport the processosr!"
#endif
	return x;
}

}
}
