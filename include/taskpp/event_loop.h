#ifndef _TASKPP_EVENT_LOOP_H_
#define _TASKPP_EVENT_LOOP_H_

#include <functional>
#include <atomic>
#include <queue>
#include <taskpp/mutex.h>
#include <taskpp/condition_variable.h>
#include <boost/thread/shared_mutex.hpp>
#include <boost/scope_exit.hpp>

namespace taskpp
{

class event_loop
{
	typedef std::function<void()> event_type;
public:
	event_loop() : status_(status_stoped), next_timer_id_(0) { }
	event_loop(const event_loop&) = delete;
	event_loop& operator=(const event_loop&) = delete;
	~event_loop()
	{
		stop();
	}

	template<typename Handler>
	void post(Handler&& handler)
	{
		{
			std::unique_lock<taskpp::mutex> lk(events_mutex_);
			events_.push(handler);
		}
		events_cv_.notify_one();
	}

	void stop(bool clear_timers=false)
	{
		if (clear_timers)
			cancel_all_timer();

		std::unique_lock<mutex> lk(events_mutex_);
		if (status_==status_running)
		{
			status_ = status_stopping;
			events_cv_.notify_all();
		}
	}
	bool stoped() const 
	{ 
		std::unique_lock<mutex> lk(events_mutex_);
		return status_==status_stoped;
	}

	void run();

	template<typename Handler>
	int set_timer(const boost::chrono::steady_clock::time_point& expire_time, Handler&& handler)
	{
		std::unique_lock<taskpp::mutex> lk(timers_mutex_);
		++next_timer_id_;
		timer_item item { next_timer_id_, boost::chrono::seconds(0), handler };
		timers_.emplace(expire_time,  std::move(item));
		timer_changed_ = true;
		return next_timer_id_;
	}

	template<typename Handler>
	int set_timer(const boost::chrono::steady_clock::duration& expire_time, bool periodic, Handler&& handler)
	{
		std::unique_lock<taskpp::mutex> lk(timers_mutex_);
		++next_timer_id_;
		auto interval=periodic ? expire_time : boost::chrono::steady_clock::duration::zero();
		timers_.emplace(boost::chrono::steady_clock::now()+expire_time, 
			timer_item { next_timer_id_, interval, handler } );
		timer_changed_ = true;
		return next_timer_id_;
	}

	bool cancel_timer(int timer_id)
	{
		std::unique_lock<mutex> lk(timers_mutex_);
		auto it=timers_.begin();
		while(it!=timers_.end())
		{
			if(it->second.id_==timer_id)
			{
				it=timers_.erase(it);
				timer_changed_ = true;
				return true;
			}
			else
			{
				++it;
			}
		}
		return false;
	}

	void cancel_all_timer()
	{
		std::unique_lock<mutex> lk(timers_mutex_);
		timers_.clear();
		timer_changed_ = true;
	}

	bool reset_timer(int timer_id, const boost::chrono::steady_clock::time_point& expire_time)
	{
		std::unique_lock<mutex> lk(timers_mutex_);
		auto it = timers_.begin();
		while (it != timers_.end())
		{
			if (it->second.id_ == timer_id)
			{
				timers_.emplace(expire_time, std::move(it->second));
				timers_.erase(it);
				timer_changed_ = true;
				return true;
			}
			else
			{
				++it;
			}
		}
		return false;
	}

	bool reset_timer(int timer_id, const boost::chrono::steady_clock::duration& expire_time)
	{
		return reset_timer(timer_id, boost::chrono::steady_clock::now() + expire_time);
	}

private:
	enum { status_stoped, status_running, status_stopping } status_;
	mutable mutex events_mutex_;
	condition_variable events_cv_;
	std::queue<event_type> events_;
	mutex timers_mutex_;
	bool timer_changed_;
	
	int next_timer_id_;
	struct timer_item
	{
		int id_;
		boost::chrono::steady_clock::duration interval_;
		event_type handler_;
	};
	std::multimap<boost::chrono::steady_clock::time_point, timer_item> timers_;

	bool pop_event(event_type& handler)
	{
		std::unique_lock<mutex> lk(events_mutex_);
		if(!events_.empty())
		{
			handler=events_.front();
			events_.pop();
			return true;
		}
		return false;
	}

	// return true if there has timer
	bool arrival_timers(boost::chrono::steady_clock::time_point& next_expired_time);

	void wait_for_timers(const boost::chrono::steady_clock::time_point& expired_time)
	{
		std::unique_lock<mutex> lk(events_mutex_);
		events_cv_.wait_until(lk, expired_time);
	}
	void wait_for_events()
	{
		std::unique_lock<mutex> lk(events_mutex_);
		events_cv_.wait(lk, [this]() {
			return !events_.empty();
		});
	}

	bool timer_changed() const
	{
		std::unique_lock<mutex> lk(events_mutex_);
		return timer_changed_;
	}
	void timer_changed(bool v)
	{
		std::unique_lock<mutex> lk(events_mutex_);
		timer_changed_=v;
	}
};

inline void event_loop::run()
{
	{
		std::unique_lock<mutex> lk(events_mutex_);
		status_ = status_running;
	}
	BOOST_SCOPE_EXIT_ALL(this) {
		std::unique_lock<mutex> lk(events_mutex_);
		status_ = status_stoped;
	};
	while(status_==status_running)
	{
		event_type handler;
		boost::chrono::steady_clock::time_point next_expired_time;
		bool has_timers=arrival_timers(next_expired_time);
		timer_changed(false);
		while(pop_event(handler))
		{
			handler();
			handler=nullptr;
			if(status_==status_stopping) return;
		}
		if (!timer_changed())
		{
			if (has_timers)
				wait_for_timers(next_expired_time);
			else
				wait_for_events();
		}
	}
}

// return true if there has timers
inline bool event_loop::arrival_timers(boost::chrono::steady_clock::time_point& next_expired_time)
{
	auto now=boost::chrono::steady_clock::now();
	std::unique_lock<mutex> lk(timers_mutex_);
	auto it=timers_.begin();
	while(it!=timers_.end())
	{
		if(it->first>now)
			break;

		post(it->second.handler_);
		if(it->second.interval_!=boost::chrono::steady_clock::duration::zero())
		{
			timers_.emplace(now+it->second.interval_, std::move(it->second));
		}
		it=timers_.erase(it);
	}

	if(!timers_.empty())
	{
		next_expired_time=timers_.begin()->first;
		return true;
	}
	else return false;
}

}

#endif //_TASKPP_EVENT_LOOP_H_
