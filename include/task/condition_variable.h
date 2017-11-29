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
		TaskBase* task = nullptr;
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

	void notify(TaskBase* task)
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
		std::vector<TaskBase*> temp;
		{
			std::unique_lock<mutex> ilk(mtx_);
			temp.swap(waiters_);
		}
		for(TaskBase* task : temp)
		{
			task->resume();
		}
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

	void wait_for(std::unique_lock<mutex>& lk, const boost::chrono::steady_clock::duration& expiry_time)
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

	void wait_until(std::unique_lock<mutex>& lk, const boost::chrono::steady_clock::time_point& expiry_time)
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
		detail::WorkThreadBase* thread = detail::ITaskScheduler::this_thread();
		if (thread)
		{
			thread->wakeup_task();
		}
	}
};

}

#endif //_TASK_CONDITION_VARIABLE_H_
