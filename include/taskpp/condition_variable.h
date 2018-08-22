#ifndef _TASKPP_CONDITION_VARIABLE_H_
#define _TASKPP_CONDITION_VARIABLE_H_

#include <vector>
#include <mutex>
#include <taskpp/mutex.h>

namespace taskpp
{

class condition_variable
{
public:
	condition_variable() = default;
	condition_variable(const condition_variable&) = delete;
	condition_variable& operator=(const condition_variable&) = delete;

	void notify_one()
	{
		base_task* task = nullptr;
		{
			std::unique_lock<mutex> ilk(mtx_);
			if (!waiters_.empty())
			{
				task = waiters_.back();
				waiters_.pop_back();
			}
		}
		if(task) task->resume();
	}

	void notify(base_task* task)
	{
		bool erased = false;
		{
			std::unique_lock<mutex> ilk(mtx_);
			if (!waiters_.empty())
			{
				auto it = std::find(waiters_.begin(), waiters_.end(), task);
				if (it != waiters_.end())
				{
					waiters_.erase(it);
					erased = true;
				}
			}
		}
		if(erased) task->resume();
	}

	void notify_all()
	{
		std::vector<base_task*> temp;
		{
			std::unique_lock<mutex> ilk(mtx_);
			temp.swap(waiters_);
		}
		for(base_task* task : temp)
		{
			task->resume();
		}
	}

	void wait(std::unique_lock<mutex>& lk)
	{
		base_task* task=this_task::self();
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

	template<typename Duration>
	void wait_for(std::unique_lock<mutex>& lk, const Duration& expiry_time)
	{
		base_task* task=this_task::self();
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

	template<typename TimePoint>
	void wait_until(std::unique_lock<mutex>& lk, const TimePoint& expiry_time)
	{
		base_task* task=this_task::self();
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
	std::vector<base_task*> waiters_;

	void add_task_to_waiter_list(base_task* task)
	{
		std::unique_lock<mutex> ilk(mtx_);
		waiters_.push_back(task);
	}
	void erase_waiter(base_task* task)
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
		detail::base_work_thread* thread = detail::base_task_scheduler::this_thread();
		if (thread)
		{
			thread->wakeup_task();
		}
	}
};

}

#endif //_TASKPP_CONDITION_VARIABLE_H_
