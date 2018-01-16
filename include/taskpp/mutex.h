#ifndef _TASKPP_MUTEX_H_
#define _TASKPP_MUTEX_H_

#include <assert.h>
#include <mutex>
#include <taskpp/task.h>

namespace taskpp
{

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
			while(!try_lock())
			{
				this_task::yield();
			}
		}
		else
		{
			mutex_.lock();
		}
	}
	bool try_lock() 
	{ 
		if(mutex_.try_lock())
		{
			task_=this_task::self();
			return true;
		}
		else return false;
	}
	void unlock() 
	{ 
		assert(task_==this_task::self());
		task_=nullptr;
		return mutex_.unlock(); 
	}

private:
	base_task* task_ { nullptr };
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

typedef basic_shared_mutex<std::mutex> shared_mutex;

}

#endif //_TASKPP_MUTEX_H_
