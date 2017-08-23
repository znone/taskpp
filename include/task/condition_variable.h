#ifndef _TASK_CONDITION_VARIABLE_H_
#define _TASK_CONDITION_VARIABLE_H_

#include <vector>
#include <mutex>
#include <task/mutex.h>

namespace task
{

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

	void notify(TaskBase* task)
	{
		std::unique_lock<mutex> ilk(mtx_);
		if(!waiters_.empty())
		{
			auto it=std::find(waiters_.begin(), waiters_.end(), task);
			if(it!=waiters_.end())
			{
				waiters_.erase(it);
				task->resume();
			}
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
	void wait(std::unique_lock<mutex>& lk, Pred pred)
	{
		while (!pred())
		{
			wait(lk);
		}
	}

	std::cv_status wait_for(std::unique_lock<mutex>& lk, const boost::chrono::steady_clock::duration& expiry_time)
	{
		return wait_until(lk, boost::chrono::steady_clock::now()+expiry_time);
	}

	template<typename Pred>
	bool wait_for(std::unique_lock<mutex>& lk, const boost::chrono::steady_clock::duration& expiry_time, Pred pred)
	{
		return wait_until(lk, boost::chrono::steady_clock::now()+expiry_time, pred);
	}

	std::cv_status wait_until(std::unique_lock<mutex>& lk, const boost::chrono::steady_clock::time_point& expiry_time)
	{
		TaskBase* task=this_task::self();
		std::cv_status status;
		if(task)
		{
			task->suspend();
			add_task_to_waiter_list(task);
			lk.unlock();
			this_task::sleep_until(expiry_time);
			if(boost::chrono::steady_clock::now()<expiry_time)
			{
				status=std::cv_status::no_timeout;
				cancel_timer();
			}
			else
			{
				status=std::cv_status::timeout;
			}
			lk.lock();
			erase_waiter(task);
		}
		else
		{
			throw invalid_task();
		}
		return status;
	}

	template<typename Pred>
	bool wait_until(std::unique_lock<mutex>& lk, const boost::chrono::steady_clock::time_point& expiry_time, Pred pred)
	{
		while(!pred())
		{
			if(wait_until(lk, expiry_time)==std::cv_status::timeout)
				return pred();
		}
		return true;
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
		detail::WorkThreadBase* thread = detail::ITaskScheduler::this_thread();
		if (thread)
		{
			thread->wakeup_task();
		}
	}
};

}

#endif //_TASK_CONDITION_VARIABLE_H_
