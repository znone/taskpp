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
#include <boost/asio/io_service.hpp>
#include <boost/asio/system_timer.hpp>
#include <boost/thread/shared_mutex.hpp>
#include <boost/coroutine/asymmetric_coroutine.hpp>

#if BOOST_VERSION<=106000
#include <boost/context/fcontext.hpp>
#else
#include <boost/context/detail/fcontext.hpp>
#endif
#include <boost/context/stack_context.hpp>
#include <boost/context/fixedsize_stack.hpp>

#include "task_queue.h"

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
	void sleep_for(const std::chrono::system_clock::duration& expiry_time);
	void sleep_until(const std::chrono::system_clock::time_point& expiry_time);
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
}

class TaskBase : public ITask
{
public:
	explicit TaskBase(int priority) 
		: priority_(priority), suspended_(false), canceled_(false), ccoroutine_(nullptr) { }
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
	void resume() { suspended_ = false;  }
	bool enabled_coroutine() const { return ccoroutine_!=nullptr; }

private:
	detail::coroutine_context* ccoroutine_;
	void yield() 
	{
		if(ccoroutine_) ccoroutine_->yield(); 
		if(canceled_) throw task_canceled();
	}
	friend void this_task::yield();

private:
	std::atomic<bool> canceled_;
	bool suspended_;
	int priority_;
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

class WorkThreadBase
{
public:
	virtual ~WorkThreadBase() { }
	TaskBase* current_task() const { return current_task_; }
	virtual void task_sleep_for(const std::chrono::system_clock::duration& expiry_time) = 0;
	virtual void task_sleep_until(const std::chrono::system_clock::time_point& expiry_time) = 0;
	virtual void wakeup_task() =0;
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

THREAD_LOCAL WorkThreadBase* ITaskScheduler::this_thread_=nullptr;

class ChainTask : public TaskBase
{
public:
	ChainTask(ITaskScheduler& schedule, int priority)
		: TaskBase(priority), schedule_(schedule)
	{
	}

	void next(TaskPtr next_task) { next_task_=next_task; }

protected:
	ITaskScheduler& schedule_;
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
	Task(ITaskScheduler& schedule, F&& f, int priority) 
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
	Task(ITaskScheduler& schedule, F&& f, int priority) 
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

	TaskSchedulerParam()
		: max_tasks_(1024), stack_size_(0)
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
	TaskScheduler()
	{
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

	size_t size() const { return task_queue_.size(); }
	bool empty() const { return task_queue_.empty(); }
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
			: scheduler_(scheduler), stack_allocator_(scheduler->param().stack_size_+sizeof(coroutine_context)),
			task_queue_(scheduler->param().max_tasks_)
		{
			stealing();
			thread_=std::thread(&WorkThread::run, this);
		}
		WorkThread(const WorkThread&) = delete;
		WorkThread& operator=(const WorkThread&) = delete;
		~WorkThread()
		{
			if(thread_.joinable()) thread_.join();
			release_stacks();
		}

		bool stoped() const { return stoped_; }
		size_t queue_size() const { return task_queue_.size(); }
		std::thread::id thread_id() const { return thread_.get_id(); }
		bool joinable() const { return thread_.joinable(); }
		void join() { thread_.join(); }
		Queue& task_queue() { return task_queue_; }

		bool push(TaskPtr task)
		{
			return task_queue_.try_push(task)==boost::queue_op_status::success;
		}

		virtual void task_sleep_for(const std::chrono::system_clock::duration& expiry_time) override
		{
			std::unique_ptr<boost::asio::system_timer> timer(new boost::asio::system_timer(service_));
			timer->expires_from_now(expiry_time);
			sleep(current_task_, std::move(timer));
		}
		virtual void task_sleep_until(const std::chrono::system_clock::time_point& expiry_time) override
		{
			std::unique_ptr<boost::asio::system_timer> timer(new boost::asio::system_timer(service_));
			timer->expires_at(expiry_time);
			sleep(current_task_, std::move(timer));
		}
		virtual void wakeup_task()
		{
			sleeping_tasks_.erase(current_task_);
		}

	private:
		typedef std::list<std::pair<TaskPtr, coroutine_context*>> executing_list;
		Queue task_queue_;
		std::map<ITask*, std::unique_ptr<boost::asio::system_timer>> sleeping_tasks_;
		bool stoped_ { false };
		TaskScheduler* scheduler_;
		boost::asio::io_service service_;
		std::thread thread_;
		StackAllocator stack_allocator_;
		std::vector<boost::context::stack_context> stacks_;

		void run()
		{
			TaskScheduler::this_thread_ = this;
			ThreadEntry entry;
			executing_list executing_tasks;
			entry;
			while (1)
			{
				TaskPtr task;
				size_t actived_count=resume_workers(executing_tasks);
				if (!pull(task, actived_count==0) && executing_tasks.empty())
					break;
				if (task)
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
				service_.run_one();
			}
			while (!executing_tasks.empty())
			{
				resume_workers(executing_tasks);
				service_.run_one();
			}
			stoped_ = true;
			TaskScheduler::this_thread_ = nullptr;
		}

		bool pull(TaskPtr& task, bool wait_for_pull)
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
					st = task_queue_.pull_for(boost::chrono::seconds(5), task);
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

		void sleep(TaskBase* task, std::unique_ptr<boost::asio::system_timer>&& timer)
		{
			auto result = sleeping_tasks_.emplace(task, std::move(timer));
			task->suspend();
			result.first->second->async_wait([this, task](const boost::system::error_code& ec) mutable {
				if (!ec) task->resume();
				sleeping_tasks_.erase(task);
				task_queue_.wakeup();
			});
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
	};

	std::vector<std::unique_ptr<WorkThread>> threads_;
	typedef std::vector<std::pair<WorkThread*, size_t>> snapshot_type;
	mutable boost::shared_mutex threads_mutex_;
	Queue task_queue_;
	TaskSchedulerParam param_;

	void adjust_threads(size_t n=1)
	{
		cleanup_threads();

		size_t thread_count=thread_size();
		for (size_t i = 0; i != n && thread_count!=param_.max_threads_; i++)
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
			auto task = create_task([]() {
				WorkThread* this_thread = static_cast<WorkThread*>(ITaskScheduler::this_thread_);
				this_thread->task_queue().close();
			});
			thread->push(task);
		}
	}

	void join_all_threads()
	{
		boost::shared_lock<boost::shared_mutex> lock(threads_mutex_);
		for(auto it=threads_.begin(); it!=threads_.end(); ++it)
		{
			std::unique_ptr<WorkThread>& thread=*it;
			if(thread->joinable()) thread->join();
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
			if (n==0 || size > n)
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
			size_t size = thread->queue_size();
			if (n == 0 || size < n)
			{
				p = thread.get();
				n = size;
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

template<class ThreadEntry=EmptyEntry, class StackAllocator=boost::context::fixedsize_stack>
class PriorityTaskScheduler : public detail::TaskScheduler <detail::sync_priority_queue<TaskPtr, std::vector<TaskPtr>, TaskPriority>, ThreadEntry, StackAllocator>
{
	typedef detail::TaskScheduler<detail::sync_priority_queue<TaskPtr, std::vector<TaskPtr>, TaskPriority>, ThreadEntry> base_;
public:
	PriorityTaskScheduler() = default;
	explicit PriorityTaskScheduler(const TaskSchedulerParam& config) : base_(config) { }
};

template<class ThreadEntry=EmptyEntry, class StackAllocator=boost::context::fixedsize_stack>
class TaskScheduler : public detail::TaskScheduler<detail::sync_queue<TaskPtr>, ThreadEntry, StackAllocator>
{
	typedef detail::TaskScheduler<detail::sync_queue<TaskPtr>, ThreadEntry> base_;
public:
	TaskScheduler() = default;
	explicit TaskScheduler(const TaskSchedulerParam& config) : base_(config) { }
};

namespace this_task
{
	TaskBase* self() { return ITaskScheduler::this_task(); }
	void yield()
	{
		TaskBase* task = ITaskScheduler::this_task();
		if (task) task->yield();
	}
	void sleep_for(const std::chrono::system_clock::duration& expiry_time)
	{
		WorkThreadBase* thread = ITaskScheduler::this_thread();
		if (thread)
		{
			thread->task_sleep_for(expiry_time);
			yield();
		}
	}

	void sleep_until(const std::chrono::system_clock::time_point& expiry_time)
	{
		WorkThreadBase* thread = ITaskScheduler::this_thread();
		if (thread)
		{
			thread->task_sleep_until(expiry_time);
			yield();
		}
	}
}

template<typename Mutex>
class basic_mutex
{
public:
	basic_mutex() = default;
	basic_mutex(const basic_mutex&) = delete;
	basic_mutex& operator=(const basic_mutex&) = delete;
	void lock()
	{
		if(this_task::self())
		{
			if(!mutex_.try_lock())
				this_task::yield();
		}
		else
		{
			mutex_.lock();
		}
	}
	bool try_lock() { return mutex_.try_lock(); }
	void unlock() { return mutex_.unlock(); }

private:
	Mutex mutex_;
};
typedef basic_mutex<std::mutex> mutex;
typedef basic_mutex<std::recursive_mutex> recursive_mutex;

template<typename Mutex>
class basic_shared_mutex
{
public:
	basic_shared_mutex() = default;
	basic_shared_mutex(const basic_shared_mutex&) = delete;
	basic_shared_mutex& operator=(const basic_shared_mutex&) = delete;
	void lock()
	{
		if(this_task::self())
		{
			if(!mutex_.try_lock())
				this_task::yield();
		}
		else
		{
			mutex_.lock();
		}
	}
	bool try_lock() { return mutex_.try_lock(); }
	void unlock() { return mutex_.unlock(); }
	void lock_shared() 
	{
		if(this_task::self())
		{
			if(!mutex_.try_lock_shared())
				this_task::yield();
		}
		else
		{
			mutex_.lock_shared();
		}
	}
	bool try_lock_shared() { return mutex_.try_lock_shared(); }
	void unlock_shared() { return mutex_.unlock_shared(); }

private:
	Mutex mutex_;
};

class condition_variable
{
public:
	condition_variable() = default;
	condition_variable(const condition_variable&) = delete;
	condition_variable& operator=(const condition_variable&) = delete;

	void notify_one()
	{
		std::unique_lock<mutex> ilk(mtx_);
		if(!waiters_.empty())
		{
			TaskBase* task=waiters_.back();
			waiters_.pop_back();
			task->resume();
		}
	}

	void notify_all()
	{
		std::unique_lock<mutex> ilk(mtx_);
		for(TaskBase* task : waiters_)
		{
			task->resume();
		}
		waiters_.clear();
	}

	void wait(std::unique_lock<mutex>& lk)
	{
		TaskBase* task=this_task::self();
		if(task)
		{
			task->suspend();
			add_task_to_waiter_list(task);
			lk.unlock();
			this_task::yield();
			lk.lock();
		}
		else
		{
			throw invalid_task();
		}
	}

	template<typename Pred>
	void wait(std::unique_lock<mutex>& lk, Pred&& pred)
	{
		while (!pred())
		{
			wait(lk);
		}
	}

	template<typename Pred>
	void wait_for(std::unique_lock<mutex>& lk, const std::chrono::system_clock::duration& expiry_time)
	{
		TaskBase* task=this_task::self();
		if(task)
		{
			task->suspend();
			add_task_to_waiter_list(task);
			lk.unlock();
			this_task::sleep_for(expiry_time);
			cancel_timer();
			lk.lock();
			erase_waiter(task);
		}
		else
		{
			throw invalid_task();
		}
	}

	template<typename Pred>
	void wait_until(std::unique_lock<mutex>& lk, const std::chrono::system_clock::time_point& expiry_time)
	{
		TaskBase* task=this_task::self();
		if(task)
		{
			task->suspend();
			add_task_to_waiter_list(task);
			lk.unlock();
			this_task::sleep_until(expiry_time);
			cancel_timer();
			lk.lock();
			erase_waiter(task);
		}
		else
		{
			throw invalid_task();
		}
	}

private:
	mutex mtx_;
	std::vector<TaskBase*> waiters_;

	void add_task_to_waiter_list(TaskBase* task)
	{
		std::unique_lock<mutex> ilk(mtx_);
		waiters_.push_back(task);
	}
	void erase_waiter(TaskBase* task)
	{
		std::unique_lock<mutex> ilk(mtx_);
		for(auto it=waiters_.begin(); it!=waiters_.end(); ++it)
		{
			if(*it==task)
			{
				waiters_.erase(it);
				break;
			}
		}
	}
	void cancel_timer()
	{
		WorkThreadBase* thread = ITaskScheduler::this_thread();
		if (thread)
		{
			thread->wakeup_task();
		}
	}
};


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
