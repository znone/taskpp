#ifndef _TASKPP_DETAIL_ASIO_H_
#define _TASKPP_DETAIL_ASIO_H_

#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <type_traits>

namespace taskpp
{

namespace detail
{

struct empty_handler { };

template<typename Handler>
struct basic_yield_context
{
	typedef std::function<void()> timeout_handler_type;

	basic_yield_context() : ec_(nullptr) { }
	explicit basic_yield_context(boost::system::error_code& ec) : ec_(&ec) { }

	basic_yield_context& operator()(const boost::chrono::steady_clock::time_point& time)
	{
		timeout_=time;
		return *this;
	}
	basic_yield_context& operator()(const boost::chrono::steady_clock::duration& duration)
	{
		timeout_=boost::chrono::steady_clock::now()+duration;
		return *this;
	}
	template<typename TimeoutHandler, 
		typename = typename std::enable_if<std::is_function<TimeoutHandler>::value>::type>
	basic_yield_context& operator()(TimeoutHandler&& handler)
	{
		timeout_handler_=handler;
		return *this;
	}
	template<typename Socket>
	basic_yield_context& operator()(Socket& socket)
	{
		timeout_handler_=[&socket]() {
			socket.cancel();
		};
		return *this;
	}

	void wait(const int& ready)
	{
		if(timeout_>=boost::chrono::steady_clock::now())
		{
			taskpp::this_task::sleep_until(timeout_, &timeout_);
		}
		else
		{
			taskpp::this_task::self()->suspend();
			taskpp::this_task::yield();
		}
		if(ready!=0) //timeout
		{
			//should cancel operation or close socket
			if(timeout_handler_) timeout_handler_();
			//continue to wait for asynchronous operation abort
			taskpp::this_task::self()->suspend();
			taskpp::this_task::yield();
		}
	}

	const boost::chrono::steady_clock::time_point* valid_time() const
	{
		return (timeout_ == boost::chrono::steady_clock::time_point()) ? nullptr : &timeout_;
	}

	boost::system::error_code* ec_;
	boost::chrono::steady_clock::time_point timeout_;
	timeout_handler_type timeout_handler_;
};

typedef basic_yield_context<empty_handler> yield_context;

void resume_asio_task(base_task* task, const boost::chrono::steady_clock::time_point*);

template<typename Handler, typename T>
class async_handler
{
public:
	async_handler(const yield_context& ctx)
		: ready_(nullptr), ctx_(const_cast<yield_context*>(&ctx)), value_(nullptr)
	{
		task_=taskpp::this_task::self();
	}

	void operator()(T&& value)
	{
		*ctx_->ec_ = boost::system::error_code();
		*value_ = std::forward<T>(value);
		if (--*ready_ == 0)
		{
			resume_asio_task(task_, ctx_->valid_time());
		}
	}

	void operator()(boost::system::error_code ec, T value)
	{
		*ctx_->ec_ = ec;
		*value_ = std::forward<T>(value);
		if (--*ready_ == 0)
		{
			resume_asio_task(task_, ctx_->valid_time());
		}
	}

	taskpp::base_task* task_;
	int* ready_;
	yield_context* ctx_;
	T* value_;
};

template<typename Handler>
class async_handler<Handler, void>
{
public:
	async_handler(const yield_context& ctx)
		: ready_(nullptr), ctx_(const_cast<yield_context*>(&ctx))
	{
		task_=taskpp::this_task::self();
	}

	void operator()()
	{
		*ctx_->ec_ = boost::system::error_code();
		if (--*ready_ == 0)
		{
			resume_asio_task(task_, ctx_->valid_time());
		}
	}

	void operator()(boost::system::error_code ec)
	{
		*ctx_->ec_ = ec;
		if (--*ready_ == 0)
		{
			resume_asio_task(task_, ctx_->valid_time());
		}
	}

	taskpp::base_task* task_;
	int* ready_;
	yield_context* ctx_;
};

class base_asio_thread : public base_work_thread
{
public:
	base_asio_thread() : service_(1) { }
	boost::asio::io_service& service() { return service_; }

	virtual void resume_task(base_task* task, const boost::chrono::steady_clock::time_point* timer)=0;

protected:
	boost::asio::io_service service_;
};

inline void resume_asio_task(base_task* task, const boost::chrono::steady_clock::time_point* timer=nullptr)
{
	base_asio_thread* thread=dynamic_cast<base_asio_thread*>(base_task_scheduler::this_thread());
	if(thread)
		thread->resume_task(task, timer);
}

template<class Queue, class ThreadEntry=empty_entry, class StackAllocator=fixedsize_stack>
class asio_thread : public work_thread<base_asio_thread, Queue, ThreadEntry, StackAllocator>
{
	typedef work_thread<base_asio_thread, Queue, ThreadEntry, StackAllocator> base;
public:
	typedef task_scheduler<Queue, ThreadEntry, StackAllocator, asio_thread> scheduler_type;

	explicit asio_thread(base_task_scheduler* scheduler)
		: base(scheduler), timer_(this->service_)
	{
	}
	~asio_thread()
	{
		if(!this->service_.stopped())
			this->service_.stop();
	}

	virtual void init() override
	{
		this->stealing();
		this->thread_ = std::thread(&asio_thread::run, this);
	}

	bool push(const task_ptr& task, bool force=false) override
	{
		if(this->task_queue_.try_push(task, force) == boost::queue_op_status::success)
		{
			this->service_.post([this]() {
			});
			return true;
		}
		return false;
	}

	virtual void run() override
	{
		typename base::markup_thread markup(this);
		ThreadEntry entry;
		boost::system::error_code ec;
		entry;
		while(1)
		{
			size_t actived_count = 0;
			boost::chrono::steady_clock::time_point suspend_time;
			do
			{
				suspend_time = this->wakeup_tasks();
				actived_count = this->resume_workers();
				if (this->task_queue_.empty())
					this->stealing();
			} while (actived_count > 0 && this->task_queue_.empty());
			//pickup task and don't blocking
			if(!this->pull(false, suspend_time))
				break;
			//run asio's handlers
			while(this->service_.poll(ec));
			//waiting for timer
			if(actived_count==0 && suspend_time>boost::chrono::steady_clock::now())
			{
				wait_for_schedule(suspend_time);
				this->service_.run_one(ec);
			}
		}
		//waiting for all tasks is stoped.
		while (!this->running_tasks_.empty())
		{
			boost::chrono::steady_clock::time_point suspend_time = this->wakeup_tasks();
			size_t actived_count= this->resume_workers();
			while(this->service_.poll(ec));
			if(suspend_time>boost::chrono::steady_clock::now())
			{
				wait_for_schedule(suspend_time);
				this->service_.run_one();
			}
		}
		this->stoped_ = true;
		this->release_stacks();
	}

	virtual void resume_task(base_task* task, const boost::chrono::steady_clock::time_point* timer) override
	{
		if(timer)
		{
			auto it=this->sleeping_tasks_.find(*timer);
			if(it!=this->sleeping_tasks_.end())
			{
				auto it_task = std::find(it->second.begin(), it->second.end(), task);
				if(it_task!=it->second.end()) it->second.erase(it_task);
				if(it->second.empty())
					this->sleeping_tasks_.erase(it);
			}
		}
		this->current_task_ = task;
		task->resume();
		if(!this->resume_coroutine(task->coroutine(), false))
		{
			auto it=this->running_tasks_.begin();
			while(it!=this->running_tasks_.end())
			{
				if(it->get()==task)
				{
					this->running_tasks_.erase(it);
					break;
				}
				++it;
			}
		}
		this->current_task_=nullptr;
	}

private:
	boost::asio::basic_waitable_timer<boost::chrono::steady_clock> timer_;

	void wait_for_schedule(const boost::chrono::steady_clock::time_point& next_time)
	{
		timer_.expires_at(next_time);
		timer_.async_wait([this](const boost::system::error_code& ec) {
		});
	}
};

}

}

namespace boost
{

namespace asio
{

template <typename Handler, typename ReturnType>
struct handler_type<taskpp::detail::basic_yield_context<Handler>, ReturnType()>
{
	typedef taskpp::detail::async_handler<Handler, void> type;
};

template <typename Handler, typename ReturnType, typename Arg1>
struct handler_type<taskpp::detail::basic_yield_context<Handler>, ReturnType(Arg1)>
{
	typedef taskpp::detail::async_handler<Handler, Arg1> type;
};

template <typename Handler, typename ReturnType>
struct handler_type<taskpp::detail::basic_yield_context<Handler>,
	ReturnType(boost::system::error_code)>
{
	typedef taskpp::detail::async_handler<Handler, void> type;
};

template <typename Handler, typename ReturnType, typename Arg2>
struct handler_type<taskpp::detail::basic_yield_context<Handler>,
	ReturnType(boost::system::error_code, Arg2)>
{
	typedef taskpp::detail::async_handler<Handler, Arg2> type;
};

template <typename Handler, typename T>
class async_result<taskpp::detail::async_handler<Handler, T> >
{
public:
	typedef T type;

	explicit async_result(taskpp::detail::async_handler<Handler, T>& h)
		: handler_(h), task_(h.task_), ctx_(*h.ctx_), ready_(2), value_(T())
	{
		handler_.ready_=&ready_;
		handler_.ctx_=&ctx_;
		out_ec_ = ctx_.ec_;
		if(!out_ec_) ctx_.ec_=&ec_;
		handler_.value_=&value_;
	}

	type&& get()
	{
		if (--ready_ != 0)
		{	// set io timeout
			ctx_.wait(ready_);
		}
		if (!out_ec_ && ec_) throw boost::system::system_error(ec_);
		return std::forward<type>(value_);
	}

private:
	taskpp::detail::async_handler<Handler, T>& handler_;
	taskpp::base_task* task_;
	int ready_;
	taskpp::detail::yield_context ctx_;
	boost::system::error_code* out_ec_;
	boost::system::error_code ec_;
	type value_;
};

template <typename Handler>
class async_result<taskpp::detail::async_handler<Handler, void> >
{
public:
	typedef void type;

	explicit async_result(taskpp::detail::async_handler<Handler, void>& h)
		: handler_(h), task_(h.task_), ctx_(*h.ctx_), ready_(2)
	{
		handler_.ready_=&ready_;
		handler_.ctx_=&ctx_;
		out_ec_ = ctx_.ec_;
		if(!out_ec_) ctx_.ec_=&ec_;
	}

	void get()
	{
		if (--ready_ != 0)
		{	// set io timeout
			ctx_.wait(ready_);
		}
		if (!out_ec_ && ec_) throw boost::system::system_error(ec_);
	}

private:
	taskpp::detail::async_handler<Handler, void>& handler_;
	taskpp::base_task* task_;
	int ready_;
	taskpp::detail::yield_context ctx_;
	boost::system::error_code* out_ec_;
	boost::system::error_code ec_;
};

}

}

#endif //_TASKPP_DETAIL_ASIO_H_
