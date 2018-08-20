#ifndef _TASKPP_TASK_H_
#define _TASKPP_TASK_H_

#include <list>
#include <thread>
#include <future>
#include <chrono>
#include <atomic>
#include <type_traits>
#include <chrono>
#include <mutex>
#include <stdint.h>
#include <boost/version.hpp>
#include <boost/thread/thread_only.hpp>
#include <boost/thread/shared_mutex.hpp>
#include <taskpp/detail/task_queue.h>
#include <taskpp/fixed_stack.h>
#include <taskpp/detail/task_coroutine.h>

#if __cplusplus>=201103L || ( defined(_MSC_VER) && _MSC_VER>=1900 )
#define THREAD_LOCAL thread_local
#elif defined(_MSC_VER)
#define THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__)
#define THREAD_LOCAL __thread
#else
#error Compiler do not supported TLS
#endif //C++11

namespace taskpp
{

namespace this_task
{
	base_task* self();
	void yield();
}

struct task_canceled : public std::exception
{
	virtual const char* what() const throw() override { return "the task is canceled."; }
};

struct invalid_task : public std::exception
{
	virtual const char* what() const throw() override { return "the function must be called in task!"; }
};

class base_task
{
public:
	explicit base_task(int priority) 
		: priority_(priority), suspended_(false), canceled_(false), 
		coroutine_(nullptr), thread_(nullptr)
	{ }
	virtual ~base_task() { }
	int priority() const { return priority_; }

	void switch_context(detail::coroutine_context* data)
	{
		coroutine_=data;
	}
	void cancel() 
	{ 
		if(!canceled())
		{
			canceled_=true; 
			if(this_task::self()==this)
				throw task_canceled();
		}
	}
	bool canceled() const { return canceled_; } 
	bool suspended() const { return suspended_; }
	void suspend() { suspended_ = true;  }
	void resume();
	bool enabled_coroutine() const { return coroutine_!=nullptr; }
	detail::coroutine_context* coroutine() { return coroutine_; }
	size_t stack_size() const { return coroutine_ ? coroutine_->stack_size() : 0; }
	void* stack_bottom() const
	{
		return coroutine_ ? coroutine_->stack_.sp : nullptr;
	}

	virtual void execute()=0;

private:
	detail::coroutine_context* coroutine_;
	void yield();
	friend void this_task::yield();

private:
	std::atomic<bool> canceled_;
	bool suspended_;
	int priority_;
	detail::base_work_thread* thread_;
};

typedef std::shared_ptr<base_task> task_ptr;

template<typename T>
inline task_ptr task_pointer_cast(const std::shared_ptr<T>& task)
{
	return std::dynamic_pointer_cast<base_task>(task);
}

struct task_priority
{
	bool operator()(const task_ptr& lhs, const task_ptr& rhs) const
	{
		if(!lhs) return false;
		else if(!rhs) return true; 
		else return lhs->priority()<rhs->priority();
	}
};

enum clock_precision
{
	seconds,
	milliseconds,
	microseconds,
	nanoseconds
};

struct task_scheduler_param
{
	size_t max_threads_;
	size_t max_tasks_ { 1024 } ;
	size_t stack_pool_capacity_ { 1024 } ;
	stack_param stack_param_;
	uint32_t alarm_remaining_ { 128 }; //If the stack remaining space is less than this value, alarm
	clock_precision clock_precision_ { milliseconds } ;
	bool enable_stealing_ { true } ;
	bool set_thread_affinity_ { false };

	task_scheduler_param()
	{
		max_threads_ = std::thread::hardware_concurrency() * 2;
	}
	task_scheduler_param(const task_scheduler_param&) = default;
};

namespace detail
{

class base_work_thread
{
public:
	virtual ~base_work_thread() { }
	base_task* current_task() const { return current_task_; }
	virtual bool push(const task_ptr& task, bool force=false)=0;
	virtual boost::chrono::steady_clock::time_point task_sleep_for(const boost::chrono::steady_clock::duration& expiry_time) = 0;
	virtual boost::chrono::steady_clock::time_point task_sleep_until(const boost::chrono::steady_clock::time_point& expiry_time) = 0;
	virtual void wakeup_task() =0;
	virtual void wakeup_queue() = 0;
	virtual void* get_stack_allocator()=0;
protected:
	base_task* current_task_ { nullptr };
};

class base_task_scheduler
{
public:
	virtual ~base_task_scheduler() { }
	virtual void push(const task_ptr& task)=0;
	virtual size_t total_queue_size()=0;
	virtual void stealing(base_work_thread* thread)=0;
	static base_work_thread* this_thread() { return this_thread_; }
	static base_task* this_task()
	{
		if(this_thread_) return this_thread_->current_task();
		else return nullptr;
	}

	const task_scheduler_param& param() const { return param_;  }

//protected:
	THREAD_LOCAL static base_work_thread* this_thread_;

protected:
	task_scheduler_param param_;
};

inline void coroutine_context::routine_implement()
{
	if (task_)
	{
		task_->execute();
	}
}

#ifdef _MSC_VER
__declspec(selectany)
#else
__attribute__((weak))
#endif
THREAD_LOCAL base_work_thread* base_task_scheduler::this_thread_=nullptr;

}

class chain_task : public base_task
{
public:
	chain_task(detail::base_task_scheduler& scheduler, int priority)
		: base_task(priority), scheduler_(scheduler)
	{
	}

	detail::base_task_scheduler& scheduler() const { return scheduler_; }
	void next(task_ptr next_task) { next_task_=next_task; }

protected:
	detail::base_task_scheduler& scheduler_;
	task_ptr next_task_;

	void push_next()
	{
		if(next_task_)
			scheduler_.push(next_task_);
	}
};

template<typename R>
class task : public chain_task, public std::enable_shared_from_this<task<R>>
{
	template<typename R1, typename A> friend class follow_task;
public:
	typedef R result_type;
	template<typename F>
	task(detail::base_task_scheduler& schedule, F&& f, int priority) 
		: chain_task(schedule, priority), task_(std::forward<F>(f))
	{
		fut_=task_.get_future();
	}

	virtual void execute() override
	{
		task_();
		push_next();
	}

	std::shared_future<R>& get_future() { return fut_; }
	
	bool finished() const
	{
		return fut_.wait_for(std::chrono::seconds(0))==std::future_status::ready;
	}
	void join()
	{
		wait_for_future();
		if(fut_.valid()) fut_.wait();
	}
	const R& result() const
	{
		wait_for_future();
		return fut_.get();
	}

	template<typename F>
	auto next(F&& f) -> std::shared_ptr<task<decltype(f())>>
	{
		typedef decltype(f()) result_type;
		std::shared_ptr<task<result_type>> new_task=std::make_shared<task<result_type>>(scheduler_, std::forward<F>(f), priority());
		next_task_=new_task;
		return new_task;
	}

	template<typename F>
	typename std::shared_ptr<task<typename std::result_of<F(R)>::type>> then(F&& f);

private:
	std::packaged_task<R()> task_;
	std::shared_future<R> fut_;

	void wait_for_future() const // valid only in the coroutine
	{
		base_task* self=this_task::self();
		if(self && self->enabled_coroutine())
		{
			while(fut_.wait_for(std::chrono::seconds(0))!=std::future_status::ready)
				this_task::yield();
		}
	}
};

template<>
class task<void> : public chain_task, public std::enable_shared_from_this<task<void>>
{
public:
	typedef void result_type;
	template<typename F>
	task(detail::base_task_scheduler& schedule, F&& f, int priority) 
		: chain_task(schedule, priority), task_(std::forward<F>(f)) 
	{
		fut_=task_.get_future();
	}
	virtual ~task() { }

	virtual void execute() override
	{
		task_();
		push_next();
	}

	std::shared_future<void>& get_future() { return fut_; }

	bool finished() const
	{
		return fut_.wait_for(std::chrono::seconds(0))==std::future_status::ready;
	}
	void join()
	{
		wait_for_future();
		if(fut_.valid()) fut_.wait();
	}
	void result() const
	{
		wait_for_future();
		fut_.get();
	}

	template<typename F>
	std::shared_ptr<task<typename std::result_of<F()>::type>> next(F&& f)
	{
		typedef typename std::result_of<F()>::type result_type;
		std::shared_ptr<task<result_type>> new_task=std::make_shared<task<result_type>>(scheduler_, std::forward<F>(f), priority());
		next_task_=new_task;
		return new_task;
	}

private:
	std::packaged_task<void()> task_;
	std::shared_future<void> fut_;

	void wait_for_future() const // valid only in the coroutine
	{
		base_task* self=dynamic_cast<base_task*>(this_task::self());
		if(self && self->enabled_coroutine())
		{
			while(fut_.wait_for(std::chrono::seconds(0))!=std::future_status::ready)
				this_task::yield();
		}
	}
};

template<typename R>
using task_shared_ptr=std::shared_ptr<task<R>>;


struct empty_entry { };

namespace detail
{

template<class Queue, class ThreadEntry, class StackAllocator, class Thread>
class task_scheduler;

template<class Parent, class Queue, class ThreadEntry=empty_entry, class StackAllocator=fixedsize_stack>
class work_thread : public Parent
{
public:
	explicit work_thread(base_task_scheduler* scheduler)
		: scheduler_(scheduler), stack_allocator_(scheduler->param().stack_param_),
		task_queue_(scheduler->param().max_tasks_)
	{
	}
	work_thread(const work_thread&) = delete;
	work_thread& operator=(const work_thread&) = delete;
	~work_thread()
	{
		if (thread_.joinable()) thread_.join();
		release_stacks();
	}

	bool stoped() const { return stoped_; }
	size_t queue_size() const { return task_queue_.size(); }
	size_t task_count() const 
	{
		size_t count = task_queue_.size()+running_tasks_.size()+sleeping_tasks_.size();
		if(this->current_task_)
			++count;
		return count;
	}
	std::thread::id thread_id() const { return thread_.get_id(); }
	bool joinable() const { return thread_.joinable(); }
	void join() { thread_.join(); }
	Queue& task_queue() { return task_queue_; }

	bool push(const task_ptr& task, bool force=false) override
	{
		return task_queue_.try_push(task, force) == boost::queue_op_status::success;
	}

	virtual void init()
	{
		stealing();
		thread_ = std::thread(&work_thread::run, this);
	}

	virtual boost::chrono::steady_clock::time_point task_sleep_for(const boost::chrono::steady_clock::duration& expiry_time) override
	{
		return task_sleep_until(boost::chrono::steady_clock::now() + expiry_time);
	}
	virtual boost::chrono::steady_clock::time_point task_sleep_until(const boost::chrono::steady_clock::time_point& expiry_time) override
	{
		//adjustment time precision
		boost::chrono::steady_clock::time_point new_time;
		switch(scheduler_->param().clock_precision_)
		{
		case seconds:
			new_time=boost::chrono::time_point_cast<boost::chrono::steady_clock::duration>(boost::chrono::time_point_cast<boost::chrono::seconds>(expiry_time));
			break;
		case milliseconds:
			new_time=boost::chrono::time_point_cast<boost::chrono::steady_clock::duration>(boost::chrono::time_point_cast<boost::chrono::milliseconds>(expiry_time));
			break;
		case microseconds:
			new_time=boost::chrono::time_point_cast<boost::chrono::steady_clock::duration>(boost::chrono::time_point_cast<boost::chrono::microseconds>(expiry_time));
			break;
		default:
			new_time=expiry_time;
		}
		auto result=sleeping_tasks_.emplace(new_time, std::vector<base_task*>());
		result.first->second.push_back(this->current_task_);
		this->current_task_->suspend();
		return new_time;
	}
	virtual void wakeup_task() override
	{
		bool found=false;
		auto it = sleeping_tasks_.begin();
		while (found==false && it != sleeping_tasks_.end())
		{
			for(auto it_task=it->second.begin(); it_task!=it->second.end(); it_task++)
			{
				if (*it_task == this->current_task_)
				{
					this->current_task_->resume();
					it->second.erase(it_task);
					found=true;
					break;
				}
			}
			if (it->second.empty())
				it = sleeping_tasks_.erase(it);
			else
				++it;
		}
	}
	virtual void wakeup_queue() override
	{
		if (task_queue_.empty())
		{
			push(std::make_shared<task<void>>(*scheduler_,
				[]() {}, 0));
		}
	}

	virtual void* get_stack_allocator() override
	{
		return &stack_allocator_;
	}

#ifdef _WIN32
	inline void set_thread_affinity(intptr_t processor)
	{
		SetThreadAffinityMask(thread_.native_handle(), 1<<processor);
	}
#else
	inline void set_thread_affinity(intptr_t processor)
	{
		cpu_set_t cs;
		CPU_ZERO(&cs);
		CPU_SET(processor, &cs);
		pthread_setaffinity_np(thread_.native_handle(), sizeof(cpu_set_t), &cs);
	}
#endif

protected:
	typedef std::deque<task_ptr> running_list;
	Queue task_queue_;
	std::map<boost::chrono::steady_clock::time_point, std::vector<base_task*>> sleeping_tasks_;
	std::atomic<bool> stoped_ { false };
	base_task_scheduler* scheduler_;
	std::thread thread_;
	StackAllocator stack_allocator_;
	std::vector<stack_context> stacks_;
	running_list running_tasks_;

	struct markup_thread
	{
		markup_thread(base_work_thread* _this) 
		{
			base_task_scheduler::this_thread_ = _this;
		}
		~markup_thread()
		{
			base_task_scheduler::this_thread_ = nullptr;
		}
	};

	virtual void run()
	{
		markup_thread markup(this);
		ThreadEntry entry;
		entry;
		while (1)
		{
			boost::chrono::steady_clock::time_point suspend_time;
			size_t actived_count = 0;
			do
			{
				suspend_time = wakeup_tasks();
				actived_count = resume_workers();
			} while (actived_count > 0 && is_all_queue_empty());
			if(!pull(actived_count==0, suspend_time) && running_tasks_.empty())
				break;
		}
		//clean up running tasks
		while (!running_tasks_.empty())
		{
			boost::chrono::steady_clock::time_point suspend_time = wakeup_tasks();
			size_t actived_count= resume_workers();
			if(actived_count==0 && !running_tasks_.empty())
				boost::this_thread::sleep_until(suspend_time);
		}
		stoped_ = true;
		//printf("thread %d is stoped\n", std::this_thread::get_id());
		release_stacks();
	}

	bool pull(bool wait_for_pull, const boost::chrono::steady_clock::time_point& suspend_time)
	{
		boost::queue_op_status st;
		if(task_queue_.empty())
			stealing();

		task_ptr task;
		st = task_queue_.try_pull(task);
		if (st == boost::queue_op_status::closed)
			return false;
		else if (st == boost::queue_op_status::empty && wait_for_pull)
		{
			try
			{
				st = task_queue_.pull_until(suspend_time, task);
			}
			catch (boost::sync_queue_is_closed&)
			{
				return false;
			}
			if (st == boost::queue_op_status::timeout)
				return false;
		}
		while(st==boost::queue_op_status::success)
		{
			start_task(task);
			task.reset();
			st=task_queue_.try_pull(task);
		}
		if (st == boost::queue_op_status::closed)
			return false;
		return true;
	}

	bool is_all_queue_empty() const
	{
		return task_queue_.empty() && scheduler_->total_queue_size()==0;
	}

	void stealing()
	{
		scheduler_->stealing(this);
	}

	//resume coroutines
	size_t resume_workers()
	{
		size_t actived_count=0;
		for(auto it=running_tasks_.begin(); it!=running_tasks_.end(); )
		{
			base_task* task=it->get();
			coroutine_context* coroutine= task->coroutine();
			if (task->suspended())
			{
				++it;
			}
			else
			{
				this->current_task_ = task;
				if(resume_coroutine(coroutine, false))
				{
					++it;
					++actived_count;
				}
				else
				{
					it=running_tasks_.erase(it);
				}
				this->current_task_=nullptr;
			}
		}
		return actived_count;
	}

	bool resume_coroutine(coroutine_context* coroutine, bool first)
	{
		if(!first)
			stack_allocator_.before_resume_task(coroutine->stack_, coroutine->mine_);
		bool running=coroutine->resume();
		if(running)
		{
			stack_allocator_.after_suspend_task(coroutine->stack_, coroutine->mine_);
		}
		else
		{
			stack_context stack=coroutine->stack_;
			if(stacks_.size()>scheduler_->param().stack_pool_capacity_)
			{
				stack_allocator_.deallocate(stack);
			}
			else
			{
				stack_allocator_.reset_stack(stack);
				stacks_.push_back(stack);
			}
		}
		return running;
	}

	void start_task(task_ptr task)
	{
		stack_context stack;
		try
		{
			stack=allocate_stack();
		}
		catch (std::bad_alloc&)
		{
			task_queue_.push(task);
			return;
		}

		if (stack.sp==nullptr)
		{
			this->current_task_ = task.get();
			try
			{
				task->execute();
			}
			catch (...)
			{
			}
			this->current_task_ = nullptr;
		}
		else
		{
			this->current_task_ = task.get();
			coroutine_context* coroutine=coroutine_context::create_coroutine(this->current_task_, stack);
			this->current_task_->switch_context(coroutine);
			if (resume_coroutine(coroutine, true))
				running_tasks_.emplace_back(task);
			this->current_task_ = nullptr;
		}
	}

	stack_context allocate_stack()
	{
		stack_context stack;
		if(stacks_.empty())
			stack=stack_allocator_.allocate();
		else
		{
			stack=stacks_.back();
			stacks_.pop_back();
		}
		return stack;
	}

	void release_stacks()
	{
		for(stack_context& stack : stacks_)
		{
			stack_allocator_.deallocate(stack);
		}
		stacks_.clear();
	}

	boost::chrono::steady_clock::time_point wakeup_tasks()
	{
		auto now = boost::chrono::steady_clock::now();
		auto next_time= now + boost::chrono::seconds(5);
		auto it=sleeping_tasks_.begin();
		size_t n=0;
		while(it!=sleeping_tasks_.end())
		{
			if(it->first>now)
				break;
			for(base_task* task : it->second)
				task->resume();
			n+=it->second.size();
			it=sleeping_tasks_.erase(it);
		}
		if(!sleeping_tasks_.empty())
		{
			it=sleeping_tasks_.begin();
			if(it->first<=next_time)
				next_time=it->first;
		}
		return next_time;
	}

};

template<class Queue, class ThreadEntry=empty_entry, class StackAllocator=fixedsize_stack,
	class Thread=work_thread<base_work_thread, Queue, ThreadEntry, StackAllocator> >
class task_scheduler : public base_task_scheduler
{
	template<class Parent, class Queue1, class ThreadEntry1, class StackAllocator1> friend class work_thread;
public:
	typedef Thread thread_type;

	typedef typename StackAllocator::traits_type stack_traits_type;
	task_scheduler()
	{
		check_param();
	}
	explicit task_scheduler(const task_scheduler_param& param)
	{
		param_=param;
		check_param();
	}
	task_scheduler(const task_scheduler&) = delete;
	task_scheduler& operator=(const task_scheduler&) = delete;
	virtual ~task_scheduler()
	{
		join_all();
		threads_.clear();
	}

	void set_param(const task_scheduler_param& param)
	{
		if (threads_.empty())
		{
			param_ = param;
			check_param();
		}
	}

	void create_threads(size_t thread_num=std::thread::hardware_concurrency())
	{
		if(task_queue_.closed()) return;
		if(thread_num>param.max_threads_) thread_num=param.max_threads_;
		for(size_t i=threads_.size(); i!=thread_num; i++)
		{
			add_thread();
		}
	}

	template<typename F>
	auto create_task(F&& f, int priority=0) -> task_shared_ptr<decltype(f())>
	{
		return std::make_shared<task<decltype(f())>>(*this, std::forward<F>(f), priority);
	}

	template<typename F>
	auto push(F&& f, int priority=0) -> task_shared_ptr<decltype(f())>
	{
		auto task=create_task(std::forward<F>(f), priority);
		assert(task);
		push(std::dynamic_pointer_cast<base_task>(task));
		return task;
	}
	template<typename F>
	auto try_push(F&& f, int priority=0) -> std::pair<task_shared_ptr<decltype(f())>, boost::queue_op_status> 
	{
		auto task=create_task(std::forward<F>(f), priority);
		assert(task);
		boost::queue_op_status status=task_queue_.try_push(std::dynamic_pointer_cast<base_task>(task));
		if(status=boost::queue_op_status::success) adjust_threads();
		return std::make_pair(task, status);
	}
	virtual void push(const task_ptr& task) override
	{
		bool pushed=false;
		if(this_thread_)
			pushed =dynamic_cast<thread_type*>(this_thread_)->push(task);
		if(!pushed)
		{
			pushed=push_to_any_thread(task);
			if(!pushed && !task_queue_.closed()) 
				task_queue_.push(task);
			adjust_threads();
		}
	}

	void join_all()
	{
		if (task_queue_.size() > thread_size() * 100)
		{
				adjust_threads(param_.max_threads_- thread_size());
		}

		dispatch_all_tasks();
		if(!task_queue_.closed())
			task_queue_.close();

		shutdown_all_threads();
		join_all_threads();
	}

private:

	std::vector<std::unique_ptr<thread_type>> threads_;
	typedef std::vector<std::pair<thread_type*, size_t>> snapshot_type;
	mutable boost::shared_mutex threads_mutex_;
	Queue task_queue_;

	virtual size_t total_queue_size() override
	{
		size_t count=task_queue_.size();
		boost::shared_lock<boost::shared_mutex> lock(threads_mutex_);
		for(std::unique_ptr<thread_type>& thread : threads_)
			count+=thread->queue_size();
		return count;
	}

	void adjust_threads(size_t n=1)
	{
		cleanup_threads();

		if(task_queue_.closed()) return;

		size_t thread_count=thread_size();
		for (size_t i = 0; i != n && thread_count<=param_.max_threads_; i++)
		{
			if( (thread_count==0 && !task_queue_.empty()) ||
				( thread_count<param_.max_threads_ && 
				(thread_count<task_queue_.size()/4 || !find_thread(std::this_thread::get_id()) )))
			{
				add_thread();
				++thread_count;
			}
		}
	}

	thread_type* add_thread()
	{
		std::unique_ptr<thread_type> thread(new thread_type(this));
		thread->init();
		boost::unique_lock<boost::shared_mutex> lock(threads_mutex_);
		threads_.push_back(std::move(thread));
		if(param_.set_thread_affinity_)
			threads_.back()->set_thread_affinity((threads_.size()-1)%std::thread::hardware_concurrency());
		return threads_.back().get();
	}

	void cleanup_threads()
	{
		boost::unique_lock<boost::shared_mutex> lock(threads_mutex_);
		for(auto it=threads_.begin(); it!=threads_.end(); )
		{
			std::unique_ptr<thread_type>& thread=*it;
			if(thread->stoped())
			{
				if(thread->joinable()) thread->join();
				it=threads_.erase(it);
			}
			else
			{
				++it;
			}
		}
	}
	size_t thread_size() const 
	{
		boost::shared_lock<boost::shared_mutex> lock(threads_mutex_);
		return threads_.size();
	}

	void shutdown_all_threads()
	{
		boost::shared_lock<boost::shared_mutex> lock(threads_mutex_);
		for (std::unique_ptr<thread_type>& thread : threads_)
		{
			if(!thread->stoped())
			{
				auto task = create_task([]() {
					thread_type* this_thread = static_cast<thread_type*>(base_task_scheduler::this_thread());
					this_thread->task_queue().close();
				});
				thread->push(task, true);
			}
		}
	}

	void join_all_threads()
	{
		boost::shared_lock<boost::shared_mutex> lock(threads_mutex_);
		for(auto it=threads_.begin(); it!=threads_.end(); ++it)
		{
			std::unique_ptr<thread_type>& thread=*it;
			if(thread->joinable())
			{
				thread->join();
				thread->task_queue().split(task_queue_, 0, 0);
			}
		}
	}
	bool find_thread(const std::thread::id& thread_id) const
	{
		boost::shared_lock<boost::shared_mutex> lock(threads_mutex_);
		for(auto it=threads_.begin(); it!=threads_.end(); it++)
		{
			const std::unique_ptr<thread_type>& thread=*it;
			if(thread->thread_id()==thread_id)
			{
				return true;
			}
		}
		return false;
	}

	std::pair<thread_type*, size_t> busiest() const
	{
		thread_type* p = nullptr;
		size_t n=0;
		for (size_t i=0; i!=threads_.size(); i++)
		{
			auto& thread = threads_[i];
			size_t size = thread->task_count();
			if (i==0 || size > n )
			{
				p = thread.get();
				n = size;
			}
		}
		return std::make_pair(p, n);
	}

	std::pair<thread_type*, size_t> most_idle() const
	{
		thread_type* p = nullptr;
		size_t n = 0;
		for (size_t i=0; i!=threads_.size(); i++)
		{
			auto& thread = threads_[i];
			if (!thread->stoped())
			{
				size_t size = thread->task_count();
				if (i==0 || size < n)
				{
					p = thread.get();
					n = size;
				}
			}
		}
		return std::make_pair(p, n);
	}

	bool push_to_any_thread(task_ptr task)
	{
		bool pushed = false;
		boost::shared_lock<boost::shared_mutex> lock(threads_mutex_);
		if (!threads_.empty())
		{
			std::pair<thread_type*, size_t> result = most_idle();
			if (result.first)
			{
				if(result.second==0 || threads_.size()>=param_.max_threads_)
					pushed = result.first->push(task);
			}
		}
		return pushed;
	}

	virtual void stealing(base_work_thread* thread) override
	{
		thread_type* current_thread=dynamic_cast<thread_type*>(thread);
		size_t n = task_queue_.split(current_thread->task_queue());
		if(n==0 && param_.enable_stealing_)
		{
			boost::shared_lock<boost::shared_mutex> lock(threads_mutex_);
			std::pair<thread_type*, size_t> result = busiest();
			if(result.first!=nullptr)
			{
				thread_type* thread = result.first;
				if (thread != current_thread)
				{
					n = thread->task_queue().split(current_thread->task_queue(), 1);
				}
			}
		}
	}

	void dispatch_all_tasks()
	{
		task_ptr task;
		boost::shared_lock<boost::shared_mutex> lock(threads_mutex_);
		if (threads_.empty()) return;
		auto it = threads_.begin();
		size_t actived=0;
		while (1)
		{
			if (task == nullptr)
			{
				if (task_queue_.try_pull(task) != boost::queue_op_status::success)
					break;
			}

			std::unique_ptr<thread_type>& thread = *it;
			if (!thread->stoped())
			{
				++actived;
				if(thread->push(task))
					task = nullptr;
			}
			++it;
			if (it == threads_.end())
			{
				if(actived==0) break;
				actived=0;
				it = threads_.begin();
			}
		}
	}

	void check_param()
	{
		if (param_.stack_param_.init_size == 0)
			param_.stack_param_.init_size = StackAllocator::default_size();
		if(param_.stack_param_.capacity==0) 
			param_.stack_param_.capacity=(uint32_t)stack_traits_type::default_size()*16;
		param_.stack_param_.reservation=sizeof(detail::coroutine_context);
		param_.stack_param_.reservation=
			(param_.stack_param_.reservation-1+stack_align)/stack_align*stack_align;
	}

};

template<typename Task, typename F1, typename... Other>
inline auto pipeline(Task&& following, F1&& f1, Other&&... other) 
	-> decltype(pipeline(following->then(std::forward<F1>(f1)), std::forward<Other>(other)...))
{
	return pipeline(following->then(std::forward<F1>(f1)), std::forward<Other>(other)...);
}

template<typename Task, typename F1>
inline task_shared_ptr<typename std::result_of<F1(typename Task::result_type)>::type> pipeline(std::shared_ptr<Task>&& following, F1&& f1)
{
	return following->then(std::forward<F1>(f1));
}

}

inline void base_task::resume()
{
	suspended_ = false;
	if (thread_ && thread_!=detail::base_task_scheduler::this_thread()) thread_->wakeup_queue();
}

inline void base_task::yield()
{
	if (coroutine_)
	{
		thread_ = detail::base_task_scheduler::this_thread();
		coroutine_->yield();
		thread_=nullptr;
	}
	if (canceled_) throw task_canceled();
}


template<class ThreadEntry=empty_entry, class StackAllocator=fixedsize_stack>
class priority_task_scheduler : public detail::task_scheduler <detail::sync_priority_queue<task_ptr, std::vector<task_ptr>, task_priority>, ThreadEntry, StackAllocator>
{
	typedef detail::task_scheduler<detail::sync_priority_queue<task_ptr, std::vector<task_ptr>, task_priority>, ThreadEntry, StackAllocator> base_;
public:
	priority_task_scheduler() = default;
	explicit priority_task_scheduler(const task_scheduler_param& param) : base_(param) { }
};

template<class ThreadEntry=empty_entry, class StackAllocator=fixedsize_stack>
class task_scheduler : public detail::task_scheduler<detail::sync_queue<task_ptr>, ThreadEntry, StackAllocator>
{
	typedef detail::task_scheduler<detail::sync_queue<task_ptr>, ThreadEntry, StackAllocator> base_;
public:
	task_scheduler() = default;
	explicit task_scheduler(const task_scheduler_param& param) : base_(param) { }
};

namespace this_task
{
	inline base_task* self() { return detail::base_task_scheduler::this_task(); }
	inline void yield()
	{
		base_task* task = detail::base_task_scheduler::this_task();
		if (task) task->yield();
	}
	inline void sleep_for(const boost::chrono::steady_clock::duration& expiry_time, boost::chrono::steady_clock::time_point* new_time=nullptr)
	{
		detail::base_work_thread* thread = detail::base_task_scheduler::this_thread();
		if (thread)
		{
			if(new_time)
				*new_time=thread->task_sleep_for(expiry_time);
			else
				thread->task_sleep_for(expiry_time);
			yield();
		}
	}

	inline void sleep_until(const boost::chrono::steady_clock::time_point& expiry_time, boost::chrono::steady_clock::time_point* new_time=nullptr)
	{
		detail::base_work_thread* thread = detail::base_task_scheduler::this_thread();
		if (thread)
		{
			if(new_time)
				*new_time=thread->task_sleep_until(expiry_time);
			else
				thread->task_sleep_until(expiry_time);
			yield();
		}
	}

	void* stack_top();
	inline size_t stack_size() { return self()->stack_size(); }
	inline size_t remaining_stack()
	{
		return (intptr_t)stack_top()-((intptr_t)self()->stack_bottom()-stack_size());
	}
	inline void check_stack_overflow()
	{
		if((intptr_t)stack_top() < ((intptr_t)self()->stack_bottom()-stack_size()))
			abort();
	}

	template<typename F>
	inline auto create_task(F&& f, int priority=0) -> task_shared_ptr<decltype(f())>
	{
		chain_task* chtask= dynamic_cast<chain_task*>(this_task::self());
		if(chtask)
		{
			return std::make_shared<task<decltype(f())>>(chtask->scheduler(), std::forward<F>(f), priority);
		}
		return nullptr;
	}


	inline bool push(const task_ptr& task)
	{
		detail::base_work_thread* thread = detail::base_task_scheduler::this_thread();
		if (thread)
			return thread->push(task, true);
		else return false;
	}

	template<typename F>
	inline auto push(F&& f, int priority=0) -> task_shared_ptr<decltype(f())>
	{
		chain_task* chtask= dynamic_cast<chain_task*>(this_task::self());
		if(chtask)
		{
			auto task=create_task(std::forward<F>(f), priority);
			assert(task);
			push(std::dynamic_pointer_cast<base_task>(task));
			return task;
		}
		return nullptr;
	}
}

template<typename R, typename A>
class follow_task : public task<R>
{
public:
	template<typename F>
	follow_task(std::shared_ptr<task<A>>&& privous, F&& f)
		: task<R>(privous->scheduler_, std::bind(&follow_task::invoke_task, this), privous->priority()),
		fun_(std::forward<F>(f))
	{
		assert(privous);
		priv_fut_=privous->get_future();
	}

private:
	std::function<R(A)> fun_;
	std::shared_future<R> priv_fut_;

	R invoke_task()
	{
		if(this->enabled_coroutine())
		{
			while(priv_fut_.wait_for(std::chrono::seconds(0))!=std::future_status::ready)
				this_task::yield();
		}
		return fun_(priv_fut_.get());
	}
};

template<typename R> template<typename F>
inline task_shared_ptr<typename std::result_of<F(R)>::type> task<R>::then(F&& f)
{
	typedef follow_task<typename std::result_of<F(R)>::type, R> result_task;
	auto task=std::make_shared<result_task>(this->shared_from_this(), std::forward<F>(f));
	next_task_=std::dynamic_pointer_cast<base_task>(task);
	return task;
}

template<typename Scheduler, typename F1, typename... Other>
inline auto pipeline(Scheduler& scheduler, F1&& f1, Other&&... other) 
	-> decltype(detail::pipeline(scheduler.create_task(std::forward<F1>(f1)), std::forward<Other>(other)...))
{
	auto first_task=scheduler.create_task(std::forward<F1>(f1));
	auto last_task=detail::pipeline(std::forward<decltype(first_task)>(first_task), std::forward<Other>(other)...);
	scheduler.push(std::dynamic_pointer_cast<base_task>(first_task));
	return last_task;
}

}

#ifdef __GNUC__
#include <taskpp/inc/stack_top_posix.inc>
#endif

#endif //_TASKPP_TASK_H_
