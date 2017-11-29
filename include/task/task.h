#ifndef _TASK_H_
#define _TASK_H_

#pragma warning(disable: 4355)

#include <list>
#include <thread>
#include <future>
#include <chrono>
#include <atomic>
#include <type_traits>
#include <chrono>
#include <mutex>
#include <boost/version.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread/shared_mutex.hpp>

#if BOOST_VERSION<=106000
#include <boost/context/fcontext.hpp>
#else
#include <boost/context/detail/fcontext.hpp>
#endif
#include <boost/context/stack_context.hpp>
#include <boost/context/fixedsize_stack.hpp>

#include "detail/task_queue.h"

#if __cplusplus>=201103L || ( defined(_MSC_VER) && _MSC_VER>=1900 )
#define THREAD_LOCAL thread_local
#elif defined(_MSC_VER)
#define THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__)
#define THREAD_LOCAL __thread
#else
#error Compiler do not supported TLS
#endif //C++11

namespace task
{

class ITask
{
public:
	virtual ~ITask() { }
	virtual void execute()=0;
};

class TaskBase;

#if BOOST_VERSION<=106000
using namespace boost::context;
#else
using namespace boost::context::detail;
#endif


namespace this_task
{
	TaskBase* self();
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

namespace detail
{
	struct coroutine_context
	{
		fcontext_t sink_;
		fcontext_t mine_;
		boost::context::stack_context stack_;
		ITask* task_;

		static coroutine_context* create_coroutine(ITask* task, boost::context::stack_context& stack)
		{
			coroutine_context* coroutine=reinterpret_cast<coroutine_context*>(stack.sp);
			--coroutine;
			new(coroutine) coroutine_context();
			coroutine->stack_=stack;
			stack.sp=coroutine;
			stack.size-=sizeof(coroutine_context);
			coroutine->sink_=nullptr;
			coroutine->mine_=make_fcontext(stack.sp, stack.size, &coroutine_context::routine);
			coroutine->task_=task;
			return coroutine;
		}

		bool resume()
		{
#if BOOST_VERSION<=106000
			return jump_fcontext(&sink_, mine_, reinterpret_cast<intptr_t>(this), false)!=0;
#else
			transfer_t transfer=jump_fcontext(mine_, this);
			mine_=transfer.fctx;
			return transfer.data!=nullptr;
#endif //BOOST_VERSION
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
			coroutine_context* _this = reinterpret_cast<coroutine_context*>(param);
			if (_this->task_)
			{
				_this->task_->execute();
			}
			jump_fcontext(&_this->mine_, _this->sink_, NULL, false);
		}
#else
		static void routine(transfer_t sink)
		{
			coroutine_context* _this = reinterpret_cast<coroutine_context*>(sink.data);
			_this->sink_=sink.fctx;
			if (_this->task_)
			{
				_this->task_->execute();
			}
			jump_fcontext(_this->sink_, NULL);
		}
#endif //BOOST_VERSION
	};

	class WorkThreadBase;
}

class TaskBase : public ITask
{
public:
	explicit TaskBase(int priority) 
		: priority_(priority), suspended_(false), canceled_(false), 
		ccoroutine_(nullptr), thread_(nullptr)
	{ }
	int priority() const { return priority_; }

	bool invoke(detail::coroutine_context* data)
	{
		ccoroutine_=data;
		return data->resume();
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
	bool enabled_coroutine() const { return ccoroutine_!=nullptr; }

private:
	detail::coroutine_context* ccoroutine_;
	void yield();
	friend void this_task::yield();

private:
	std::atomic<bool> canceled_;
	bool suspended_;
	int priority_;
	detail::WorkThreadBase* thread_;
};

typedef std::shared_ptr<TaskBase> TaskPtr;

template<typename T>
inline TaskPtr task_pointer_cast(const std::shared_ptr<T>& task)
{
	return std::dynamic_pointer_cast<TaskBase>(task);
}

struct TaskPriority
{
	bool operator()(const TaskPtr& lhs, const TaskPtr& rhs) const
	{
		if(!lhs) return false;
		else if(!rhs) return true; 
		else return lhs->priority()<rhs->priority();
	}
};

namespace detail
{

class WorkThreadBase
{
public:
	virtual ~WorkThreadBase() { }
	TaskBase* current_task() const { return current_task_; }
	virtual void task_sleep_for(const boost::chrono::steady_clock::duration& expiry_time) = 0;
	virtual void task_sleep_until(const boost::chrono::steady_clock::time_point& expiry_time) = 0;
	virtual void wakeup_task() =0;
	virtual void wakeup_queue() = 0;
protected:
	TaskBase* current_task_ { nullptr };
};

class ITaskScheduler
{
public:
	virtual ~ITaskScheduler() { }
	virtual void push(TaskPtr task)=0;
	static WorkThreadBase* this_thread() { return this_thread_; }
	static TaskBase* this_task()
	{
		if(this_thread_) return this_thread_->current_task();
		else return nullptr;
	}
protected:
	THREAD_LOCAL static WorkThreadBase* this_thread_;
};

#ifdef _MSC_VER
__declspec(selectany)
#else
__attribute__((weak))
#endif
THREAD_LOCAL WorkThreadBase* ITaskScheduler::this_thread_=nullptr;

}

class ChainTask : public TaskBase
{
public:
	ChainTask(detail::ITaskScheduler& schedule, int priority)
		: TaskBase(priority), schedule_(schedule)
	{
	}

	void next(TaskPtr next_task) { next_task_=next_task; }

protected:
	detail::ITaskScheduler& schedule_;
	TaskPtr next_task_;

	void push_next()
	{
		if(next_task_)
			schedule_.push(next_task_);
	}
};

template<typename R>
class Task : public ChainTask, public std::enable_shared_from_this<Task<R>>
{
	template<typename R1, typename A> friend class FollowTask;
public:
	typedef R result_type;
	template<typename F>
	Task(detail::ITaskScheduler& schedule, F&& f, int priority) 
		: ChainTask(schedule, priority), task_(std::forward<F>(f))
	{
		fut_=task_.get_future();
	}

	virtual void execute() override
	{
		task_();
		push_next();
	}

	std::shared_future<R>& get_future() { return fut_; }
	
	void join()
	{
		wait_for_future();
		return fut_.wait();
	}
	R result()
	{
		wait_for_future();
		return fut_.get();
	}

	template<typename F>
	auto next(F&& f) -> std::shared_ptr<Task<decltype(f())>>
	{
		typedef decltype(f()) result_type;
		std::shared_ptr<Task<result_type>> new_task=std::make_shared<Task<result_type>>(schedule_, std::forward<F>(f), priority());
		next_task_=new_task;
		return new_task;
	}

	template<typename F>
	typename std::shared_ptr<Task<typename std::result_of<F(R)>::type>> then(F&& f);

private:
	std::packaged_task<R()> task_;
	std::shared_future<R> fut_;

	void wait_for_future() // valid only in the coroutine
	{
		TaskBase* self=this_task::self();
		if(self && self->enabled_coroutine())
		{
			while(fut_.wait_for(std::chrono::seconds(0))!=std::future_status::ready)
				this_task::yield();
		}
	}
};

template<>
class Task<void> : public ChainTask, public std::enable_shared_from_this<Task<void>>
{
public:
	typedef void result_type;
	template<typename F>
	Task(detail::ITaskScheduler& schedule, F&& f, int priority) 
		: ChainTask(schedule, priority), task_(std::forward<F>(f)) 
	{
		fut_=task_.get_future();
	}

	virtual void execute() override
	{
		task_();
		push_next();
	}

	std::shared_future<void>& get_future() { return fut_; }

	void join()
	{
		wait_for_future();
		fut_.wait();
	}
	void result()
	{
		wait_for_future();
		fut_.get();
	}

	template<typename F>
	std::shared_ptr<Task<typename std::result_of<F()>::type>> next(F&& f)
	{
		typedef typename std::result_of<F()>::type result_type;
		std::shared_ptr<Task<result_type>> new_task=std::make_shared<Task<result_type>>(schedule_, std::forward<F>(f), priority());
		next_task_=new_task;
		return new_task;
	}

private:
	std::packaged_task<void()> task_;
	std::shared_future<void> fut_;

	void wait_for_future() // valid only in the coroutine
	{
		TaskBase* self=dynamic_cast<TaskBase*>(this_task::self());
		if(self && self->enabled_coroutine())
		{
			while(fut_.wait_for(std::chrono::seconds(0))!=std::future_status::ready)
				this_task::yield();
		}
	}
};

template<typename R>
using TaskSharedPtr=std::shared_ptr<Task<R>>;

struct EmptyEntry { };

struct TaskSchedulerParam
{
	size_t max_threads_;
	size_t max_tasks_;
	size_t stack_size_;
	bool disable_coroutine_;

	TaskSchedulerParam()
		: max_tasks_(1024), stack_size_(0), disable_coroutine_(false)
	{
		max_threads_ = std::thread::hardware_concurrency() * 2;
	}
	TaskSchedulerParam(const TaskSchedulerParam&) = default;
};

namespace detail
{

template<class Queue, class ThreadEntry=EmptyEntry, class StackAllocator=boost::context::fixedsize_stack>
class TaskScheduler : public ITaskScheduler
{
public:
	typedef typename StackAllocator::traits_type stack_traits_type;
	TaskScheduler(bool disable_coroutine=false)
	{
		param_.disable_coroutine_ = disable_coroutine;
		if(param_.stack_size_==0) 
			param_.stack_size_=stack_traits_type::default_size();
	}
	TaskScheduler(const TaskSchedulerParam& param)
		: param_(param)
	{
		if(param_.stack_size_==0) 
			param_.stack_size_=stack_traits_type::default_size();
	}
	TaskScheduler(const TaskScheduler&) = delete;
	TaskScheduler& operator=(const TaskScheduler&) = delete;
	virtual ~TaskScheduler()
	{
		join_all();
		threads_.clear();
	}

	const TaskSchedulerParam& param() const { return param_;  }

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
	auto create_task(F&& f, int priority=0) -> TaskSharedPtr<decltype(f())>
	{
		return std::make_shared<Task<decltype(f())>>(*this, std::forward<F>(f), priority);
	}

	template<typename F>
	auto push(F&& f, int priority=0) -> TaskSharedPtr<decltype(f())>
	{
		auto task=create_task(std::forward<F>(f), priority);
		assert(task);
		push(std::dynamic_pointer_cast<TaskBase>(task));
		return task;
	}
	template<typename F>
	auto try_push(F&& f, int priority=0) -> std::pair<TaskSharedPtr<decltype(f())>, boost::queue_op_status> 
	{
		auto task=create_task(std::forward<F>(f), priority);
		assert(task);
		boost::queue_op_status status=task_queue_.try_push(std::dynamic_pointer_cast<ITask>(task));
		if(status=boost::queue_op_status::success) adjust_threads();
		return std::make_pair(task, status);
	}
	virtual void push(TaskPtr task) override
	{
		bool pushed=false;
		if(this_thread_)
			pushed =dynamic_cast<WorkThread*>(this_thread_)->push(task);
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
	class WorkThread : public WorkThreadBase
	{
	public:
		WorkThread(TaskScheduler* scheduler)
			: scheduler_(scheduler), stack_allocator_(scheduler->param().stack_size_ + sizeof(coroutine_context)),
			task_queue_(scheduler->param().max_tasks_)
		{
			stealing();
			thread_ = std::thread(&WorkThread::run, this);
		}
		WorkThread(const WorkThread&) = delete;
		WorkThread& operator=(const WorkThread&) = delete;
		~WorkThread()
		{
			if (thread_.joinable()) thread_.join();
			release_stacks();
		}

		bool stoped() const { return stoped_; }
		size_t queue_size() const { return task_queue_.size(); }
		std::thread::id thread_id() const { return thread_.get_id(); }
		bool joinable() const { return thread_.joinable(); }
		void join() { thread_.join(); }
		Queue& task_queue() { return task_queue_; }

		bool push(TaskPtr task, bool force=false)
		{
			return task_queue_.try_push(task, force) == boost::queue_op_status::success;
		}

		virtual void task_sleep_for(const boost::chrono::steady_clock::duration& expiry_time) override
		{
			task_sleep_until(boost::chrono::steady_clock::now() + expiry_time);
		}
		virtual void task_sleep_until(const boost::chrono::steady_clock::time_point& expiry_time) override
		{
			sleeping_tasks_.emplace(expiry_time, current_task_);
		}
		virtual void wakeup_task()
		{
			for (auto it = sleeping_tasks_.begin(); it != sleeping_tasks_.end(); it++)
			{
				if (it->second == current_task_)
				{
					current_task_->resume();
					sleeping_tasks_.erase(it);
					break;
				}
			}
		}
		virtual void wakeup_queue()
		{
			if (task_queue_.empty())
			{
				push(std::make_shared<Task<void>>(*scheduler_,
					[]() {}, 0));
			}
		}

	private:
		typedef std::list<std::pair<TaskPtr, coroutine_context*>> executing_list;
		Queue task_queue_;
		std::multimap<boost::chrono::steady_clock::time_point, TaskBase*> sleeping_tasks_;
		std::atomic<bool> stoped_ { false };
		TaskScheduler* scheduler_;
		std::thread thread_;
		StackAllocator stack_allocator_;
		std::vector<boost::context::stack_context> stacks_;

		void run()
		{
			TaskScheduler::this_thread_ = this;
			ThreadEntry entry;
			executing_list executing_tasks;
			entry;
			//printf("thread %d is created\n", std::this_thread::get_id());
			while (1)
			{
				TaskPtr task;
				boost::chrono::steady_clock::time_point suspend_time;
				size_t actived_count = 0;
				do
				{
					suspend_time = wakeup_tasks();
					size_t actived_count = resume_workers(executing_tasks);
					if (task_queue_.empty())
						stealing();
				} while (actived_count > 0 && task_queue_.empty());
				if (!pull(task, actived_count==0, suspend_time) && executing_tasks.empty())
					break;
				if (task)
				{
					if (scheduler_->param().disable_coroutine_)
					{
						current_task_ = task.get();
						try
						{
							task->execute();
						}
						catch (...)
						{
						}
						current_task_ = nullptr;
					}
					else
					{
						boost::context::stack_context stack=allocate_stack();
						current_task_ = task.get();
						coroutine_context* coroutine=coroutine_context::create_coroutine(current_task_, stack);
						if (current_task_->invoke(coroutine))
						{
							executing_tasks.emplace_back(task, coroutine);
						}
						else
						{
							stacks_.push_back(coroutine->stack_);
						}
						current_task_ = nullptr;
					}
					if(task_queue_.closed())
						break;
				}
			}
			//clean up running tasks
			while (!executing_tasks.empty())
			{
				boost::chrono::steady_clock::time_point suspend_time = wakeup_tasks();
				size_t actived_count= resume_workers(executing_tasks);
				if(actived_count==0 && !executing_tasks.empty())
					boost::this_thread::sleep_until(suspend_time);
			}
			stoped_ = true;
			//printf("thread %d is stoped\n", std::this_thread::get_id());
			TaskScheduler::this_thread_ = nullptr;
		}

		bool pull(TaskPtr& task, bool wait_for_pull, const boost::chrono::steady_clock::time_point& suspend_time)
		{
			boost::queue_op_status st;
			if(task_queue_.empty())
				stealing();

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
			}
			if (st == boost::queue_op_status::closed || st == boost::queue_op_status::timeout)
				return false;
			return true;
		}

		void stealing()
		{
			scheduler_->stealing(this);
		}

		//resume coroutines
		size_t resume_workers(executing_list& workers)
		{
			size_t actived_count=0;
			for(auto it=workers.begin(); it!=workers.end(); )
			{
				coroutine_context* coroutine=it->second;
				TaskBase* task=it->first.get();
				if (task->suspended())
				{
					++it;
				}
				else
				{
					current_task_ = task;
					bool running=coroutine->resume();
					current_task_=nullptr;
					if(!running)
					{
						it=workers.erase(it);
						stacks_.push_back(coroutine->stack_);
					}
					else
					{
						++it;
						++actived_count;
					}
				}
			}
			return actived_count;
		}

		boost::context::stack_context allocate_stack()
		{
			boost::context::stack_context stack;
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
			for(boost::context::stack_context& stack : stacks_)
			{
				stack_allocator_.deallocate(stack);
			}
		}

		boost::chrono::steady_clock::time_point wakeup_tasks()
		{
			auto now = boost::chrono::steady_clock::now();
			auto next_time= now + boost::chrono::seconds(5);
			auto it=sleeping_tasks_.begin();
			while(it!=sleeping_tasks_.end())
			{
				if(it->first>now)
					break;

				it->second->resume();
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

	std::vector<std::unique_ptr<WorkThread>> threads_;
	typedef std::vector<std::pair<WorkThread*, size_t>> snapshot_type;
	mutable boost::shared_mutex threads_mutex_;
	Queue task_queue_;
	TaskSchedulerParam param_;

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

	void add_thread()
	{
		std::unique_ptr<WorkThread> thread(new WorkThread(this));
		boost::unique_lock<boost::shared_mutex> lock(threads_mutex_);
		threads_.push_back(std::move(thread));
	}

	void cleanup_threads()
	{
		boost::unique_lock<boost::shared_mutex> lock(threads_mutex_);
		for(auto it=threads_.begin(); it!=threads_.end(); )
		{
			std::unique_ptr<WorkThread>& thread=*it;
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
		for (std::unique_ptr<WorkThread>& thread : threads_)
		{
			if(!thread->stoped())
			{
				auto task = create_task([]() {
					WorkThread* this_thread = static_cast<WorkThread*>(ITaskScheduler::this_thread());
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
			std::unique_ptr<WorkThread>& thread=*it;
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
			const std::unique_ptr<WorkThread>& thread=*it;
			if(thread->thread_id()==thread_id)
			{
				return true;
			}
		}
		return false;
	}

	std::pair<WorkThread*, size_t> busiest() const
	{
		WorkThread* p = nullptr;
		size_t n=0;
		for (const std::unique_ptr<WorkThread>& thread : threads_)
		{
			size_t size = thread->queue_size();
			if (size>0 && (n==0 || size > n) )
			{
				p = thread.get();
				n = size;
			}
		}
		return std::make_pair(p, n);
	}

	std::pair<WorkThread*, size_t> most_idle() const
	{
		WorkThread* p = nullptr;
		size_t n = 0;
		for (const std::unique_ptr<WorkThread>& thread : threads_)
		{
			if (!thread->stoped())
			{
				size_t size = thread->queue_size();
				if (n == 0 || size < n)
				{
					p = thread.get();
					n = size;
				}
			}
		}
		return std::make_pair(p, n);
	}

	bool push_to_any_thread(TaskPtr task)
	{
		bool pushed = false;
		boost::shared_lock<boost::shared_mutex> lock(threads_mutex_);
		if (!threads_.empty())
		{
			std::pair<WorkThread*, size_t> result = most_idle();
			if (result.first)
			{
				pushed = result.first->push(task);
			}
		}
		return pushed;
	}

	void stealing(WorkThread* current_thread)
	{
		size_t n = task_queue_.split(current_thread->task_queue());
		if(n==0)
		{
			boost::shared_lock<boost::shared_mutex> lock(threads_mutex_);
			std::pair<WorkThread*, size_t> result = busiest();
			if(result.first!=nullptr)
			{
				WorkThread* thread = result.first;
				if (thread != current_thread)
				{
					n = thread->task_queue().split(current_thread->task_queue(), 1);
				}
			}
		}
	}

	void dispatch_all_tasks()
	{
		TaskPtr task;
		boost::shared_lock<boost::shared_mutex> lock(threads_mutex_);
		if (threads_.empty()) return;
		auto it = threads_.begin();
		while (1)
		{
			if (task == nullptr)
			{
				if (task_queue_.try_pull(task) != boost::queue_op_status::success)
					break;
			}

			std::unique_ptr<WorkThread>& thread = *it;
			if (thread->push(task))
				task = nullptr;
			++it;
			if (it == threads_.end())
				it = threads_.begin();
		}
	}

};

template<typename Task, typename F1, typename... Other>
inline auto pipeline(Task&& task, F1&& f1, Other&&... other) 
	-> decltype(pipeline(task->then(std::forward<F1>(f1)), std::forward<Other>(other)...))
{
	return pipeline(task->then(std::forward<F1>(f1)), std::forward<Other>(other)...);
}

template<typename Task, typename F1>
inline TaskSharedPtr<typename std::result_of<F1(typename Task::result_type)>::type> pipeline(std::shared_ptr<Task>&& task, F1&& f1) 
{
	return task->then(std::forward<F1>(f1));
}

}

inline void TaskBase::resume()
{
	suspended_ = false;
	if (thread_ && thread_!=detail::ITaskScheduler::this_thread()) thread_->wakeup_queue();
}

inline void TaskBase::yield()
{
	if (ccoroutine_)
	{
		thread_ = detail::ITaskScheduler::this_thread();
		ccoroutine_->yield();
		thread_=nullptr;
	}
	if (canceled_) throw task_canceled();
}


template<class ThreadEntry=EmptyEntry, class StackAllocator=boost::context::fixedsize_stack>
class PriorityTaskScheduler : public detail::TaskScheduler <detail::sync_priority_queue<TaskPtr, std::vector<TaskPtr>, TaskPriority>, ThreadEntry, StackAllocator>
{
	typedef detail::TaskScheduler<detail::sync_priority_queue<TaskPtr, std::vector<TaskPtr>, TaskPriority>, ThreadEntry> base_;
public:
	PriorityTaskScheduler(bool disable_coroutine = false) : base_(disable_coroutine)  { }
	explicit PriorityTaskScheduler(const TaskSchedulerParam& config) : base_(config) { }
};

template<class ThreadEntry=EmptyEntry, class StackAllocator=boost::context::fixedsize_stack>
class TaskScheduler : public detail::TaskScheduler<detail::sync_queue<TaskPtr>, ThreadEntry, StackAllocator>
{
	typedef detail::TaskScheduler<detail::sync_queue<TaskPtr>, ThreadEntry> base_;
public:
	TaskScheduler(bool disable_coroutine = false) : base_(disable_coroutine) { }
	explicit TaskScheduler(const TaskSchedulerParam& config) : base_(config) { }
};

struct io_result
{
	boost::system::error_code error;
	std::size_t bytes_transferred { 0 };
};

namespace this_task
{
	inline TaskBase* self() { return detail::ITaskScheduler::this_task(); }
	inline void yield()
	{
		TaskBase* task = detail::ITaskScheduler::this_task();
		if (task) task->yield();
	}
	inline void sleep_for(const boost::chrono::steady_clock::duration& expiry_time)
	{
		detail::WorkThreadBase* thread = detail::ITaskScheduler::this_thread();
		if (thread)
		{
			thread->task_sleep_for(expiry_time);
			yield();
		}
	}

	inline void sleep_until(const boost::chrono::steady_clock::time_point& expiry_time)
	{
		detail::WorkThreadBase* thread = detail::ITaskScheduler::this_thread();
		if (thread)
		{
			thread->task_sleep_until(expiry_time);
			yield();
		}
	}

	template<typename Function, typename... Args>
	inline io_result await_io(Function&& fun, Args&&... args)
	{
		TaskBase* task=this_task::self();
		io_result result;
		fun(std::forward<Args>(args)..., [task, &result](const boost::system::error_code& error, std::size_t bytes_transferred ) mutable {
			result.error=error;
			result.bytes_transferred=bytes_transferred;
			task->resume();
		});
		task->suspend();
		yield();
		return result;
	}
}


template<typename R, typename A>
class FollowTask : public Task<R>
{
public:
	template<typename F>
	FollowTask(std::shared_ptr<Task<A>>&& privous, F&& f)
		: Task<R>(privous->schedule_, std::bind(&FollowTask::invoke_task, this), privous->priority()),
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
inline TaskSharedPtr<typename std::result_of<F(R)>::type> Task<R>::then(F&& f)
{
	typedef FollowTask<typename std::result_of<F(R)>::type, R> result_task;
	auto task=std::make_shared<result_task>(this->shared_from_this(), std::forward<F>(f));
	next_task_=std::dynamic_pointer_cast<TaskBase>(task);
	return task;
}

template<typename Scheduler, typename F1, typename... Other>
inline auto pipeline(Scheduler& scheduler, F1&& f1, Other&&... other) 
	-> decltype(detail::pipeline(scheduler.create_task(std::forward<F1>(f1)), std::forward<Other>(other)...))
{
	auto first_task=scheduler.create_task(std::forward<F1>(f1));
	auto last_task=detail::pipeline(std::forward<decltype(first_task)>(first_task), std::forward<Other>(other)...);
	scheduler.push(std::dynamic_pointer_cast<TaskBase>(first_task));
	return last_task;
}

}

#endif //_TASK_H_
