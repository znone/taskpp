#ifndef TASK_BLOCKING_QUEUE_H_
#define TASK_BLOCKING_QUEUE_H_

#include <deque>
#include <taskpp/mutex.h>
#include <taskpp/condition_variable.h>
#include <boost/thread/concurrent_queues/queue_op_status.hpp>

namespace taskpp
{

template<typename T>
class blocking_message
{
public:
	typedef T value_type;
	blocking_message() 
		: data_(nullptr), closed_(false), sender_(nullptr),
		waiting_empty_(0), waiting_full_(0) 
	{
	}
	blocking_message(const blocking_message&) = delete;
	blocking_message& operator=(const blocking_message&) = delete;

	void push(const value_type& v)
	{
		std::unique_lock<mutex> lk(mtx_);
		base_task* task=this_task::self();
		//only enable one sender
		while(sender_!=nullptr && sender_!=task)
		{
			task->suspend();
			pending_sender.push_back(task);
			lk.unlock();
			this_task::yield();
			lk.lock();
		}
		data_=&v;
		sender_=task;
		if(notify_not_empty_if_needed(lk))
			lk.lock();
		try
		{
			wait_until_not_full(lk);
		}
		catch (boost::sync_queue_is_closed&)
		{
			sender_=nullptr;
			for(base_task* task : pending_sender)
				task->resume();
			pending_sender.clear();
			throw;
		}
		
		sender_=nullptr;
		//wakeup a pending sender
		if(closed_)
		{
			for(base_task* task : pending_sender)
				task->resume();
			pending_sender.clear();
		}
		else if(!pending_sender.empty())
		{
			pending_sender.front()->resume();
			pending_sender.pop_front();
		}
	}
	value_type pull()
	{
		value_type v;
		pull(v);
		return v;
	}
	void pull(value_type& v)
	{
		std::unique_lock<mutex> lk(mtx_);
		wait_until_not_empty(lk);
		v=*data_;
		data_=nullptr;
		notify_not_full_if_needed(lk);
	}

	void close()
	{
		{
			std::lock_guard<mutex> lk(mtx_);
			closed_ = true;
		}
		not_empty_.notify_all();
		not_full_.notify_all();
	}
 
private:
	mutable mutex mtx_;
	condition_variable not_empty_;
	condition_variable not_full_;
	const value_type* data_;
	bool closed_;
	size_t waiting_empty_;
	size_t waiting_full_;
	base_task* sender_;
	std::deque<base_task*> pending_sender;

	bool full() const { return data_!=nullptr; }
	inline bool notify_not_empty_if_needed(std::unique_lock<mutex>& lk)
	{
		if (waiting_empty_ > 0)
		{
			--waiting_empty_;
			lk.unlock();
			not_empty_.notify_one();
			return true;
		}
		return false;
	}
	inline bool notify_not_full_if_needed(std::unique_lock<mutex>& lk)
	{
		if (waiting_full_ > 0)
		{
			--waiting_full_;
			lk.unlock();
			if(sender_)
				not_full_.notify(sender_);
			else
				not_full_.notify_one();
			return true;
		}
		return false;
	}

	void wait_until_not_full(std::unique_lock<mutex>& lk)
	{
		for (;;)
		{
			throw_if_closed(lk);
			if (!full())
			{
				return;
			}
			++waiting_full_;
			not_full_.wait(lk);
		}
	}

	void wait_until_not_empty(std::unique_lock<mutex>& lk)
	{
		for (;;)
		{
			throw_if_closed(lk);
			if (full()) break;
			++waiting_empty_;
			not_empty_.wait(lk);
		}
	}

	void throw_if_closed(std::unique_lock<mutex>&)
	{
		if (closed_)
		{
			throw boost::sync_queue_is_closed();
		}
	}
};

template <typename T>
class blocking_queue
{
public:
	typedef T value_type;
	typedef std::size_t size_type;

	// Constructors/Assignment/Destructors
	explicit blocking_queue(size_type max_elems);
	blocking_queue(const blocking_queue&) = delete;
	blocking_queue& operator=(const blocking_queue&) = delete;
	~blocking_queue();

	// Observers
	inline bool empty() const;
	inline bool full() const;
	inline size_type capacity() const;
	inline size_type size() const;
	inline bool closed() const;

	// Modifiers
	inline void close();

	inline void push_back(const value_type& x);
	inline void push_back(value_type&& x);
	inline boost::queue_op_status try_push_back(const value_type& x);
	inline boost::queue_op_status try_push_back(value_type&& x);
	inline boost::queue_op_status nonblocking_push_back(const value_type& x);
	inline boost::queue_op_status nonblocking_push_back(value_type&& x);
	inline boost::queue_op_status wait_push_back(const value_type& x);
	inline boost::queue_op_status wait_push_back(value_type&& x);

	inline void pull_front(value_type&);
	// enable_if is_nothrow_copy_movable<value_type>
	inline value_type pull_front();
	inline boost::queue_op_status try_pull_front(value_type&);
	inline boost::queue_op_status nonblocking_pull_front(value_type&);

	inline boost::queue_op_status wait_pull_front(T& elem);

private:
	mutable mutex mtx_;
	condition_variable not_empty_;
	condition_variable not_full_;
	size_type waiting_full_;
	size_type waiting_empty_;
	value_type* data_;
	size_type in_;
	size_type out_;
	size_type capacity_;
	bool closed_;

	inline size_type inc(size_type idx) const BOOST_NOEXCEPT
	{
	  return (idx + 1) % capacity_;
	}

	inline bool empty(std::unique_lock<mutex>& ) const BOOST_NOEXCEPT
	{
	  return in_ == out_;
	}
	inline bool empty(std::lock_guard<mutex>& ) const BOOST_NOEXCEPT
	{
	  return in_ == out_;
	}
	inline bool full(std::unique_lock<mutex>& ) const BOOST_NOEXCEPT
	{
	  return (inc(in_) == out_);
	}
	inline bool full(std::lock_guard<mutex>& ) const BOOST_NOEXCEPT
	{
	  return (inc(in_) == out_);
	}
	inline size_type capacity(std::lock_guard<mutex>& ) const BOOST_NOEXCEPT
	{
	  return capacity_-1;
	}
	inline size_type size(std::lock_guard<mutex>& lk) const BOOST_NOEXCEPT
	{
	  if (full(lk)) return capacity(lk);
	  return ((out_+capacity(lk)-in_) % capacity(lk));
	}

	inline void throw_if_closed(std::unique_lock<mutex>&);
	inline bool closed(std::unique_lock<mutex>&) const;

	inline boost::queue_op_status try_pull_front(value_type& x, std::unique_lock<mutex>& lk);
	inline boost::queue_op_status try_push_back(const value_type& x, std::unique_lock<mutex>& lk);
	inline boost::queue_op_status try_push_back(value_type&& x, std::unique_lock<mutex>& lk);

	inline boost::queue_op_status wait_pull_front(value_type& x, std::unique_lock<mutex>& lk);
	inline boost::queue_op_status wait_push_back(const value_type& x, std::unique_lock<mutex>& lk);
	inline boost::queue_op_status wait_push_back(value_type&& x, std::unique_lock<mutex>& lk);

	inline void wait_until_not_empty(std::unique_lock<mutex>& lk);
	inline void wait_until_not_empty(std::unique_lock<mutex>& lk, bool&);
	inline size_type wait_until_not_full(std::unique_lock<mutex>& lk);
	inline size_type wait_until_not_full(std::unique_lock<mutex>& lk, bool&);

	inline void notify_not_empty_if_needed(std::unique_lock<mutex>& lk)
	{
	  if (waiting_empty_ > 0)
	  {
		--waiting_empty_;
		lk.unlock();
		not_empty_.notify_one();
	  }
	}
	inline void notify_not_full_if_needed(std::unique_lock<mutex>& lk)
	{
	  if (waiting_full_ > 0)
	  {
		--waiting_full_;
		lk.unlock();
		not_full_.notify_one();
	  }
	}

	inline void pull_front(value_type& elem, std::unique_lock<mutex>& lk)
	{
	  elem = std::move(data_[out_]);
	  out_ = inc(out_);
	  notify_not_full_if_needed(lk);
	}
	inline value_type pull_front(std::unique_lock<mutex>& lk)
	{
	  value_type elem = std::move(data_[out_]);
	  out_ = inc(out_);
	  notify_not_full_if_needed(lk);
	  return std::move(elem);
	}

	inline void set_in(size_type in, std::unique_lock<mutex>& lk)
	{
	  in_ = in;
	  notify_not_empty_if_needed(lk);
	}

	inline void push_at(const value_type& elem, size_type in_p_1, std::unique_lock<mutex>& lk)
	{
	  data_[in_] = elem;
	  set_in(in_p_1, lk);
	}

	inline void push_at(value_type&& elem, size_type in_p_1, std::unique_lock<mutex>& lk)
	{
	  data_[in_] = std::move(elem);
	  set_in(in_p_1, lk);
	}
};

template <typename T>
blocking_queue<T>::blocking_queue(typename blocking_queue<T>::size_type max_elems) :
	waiting_full_(0), waiting_empty_(0), data_(new value_type[max_elems + 1]), in_(0), out_(0), capacity_(max_elems + 1),
    closed_(false)
{
}

template <typename T>
blocking_queue<T>::~blocking_queue()
{
	delete[] data_;
}

template <typename T>
void blocking_queue<T>::close()
{
	{
		std::lock_guard<mutex> lk(mtx_);
		closed_ = true;
	}
	not_empty_.notify_all();
	not_full_.notify_all();
}

template <typename T>
bool blocking_queue<T>::closed() const
{
	std::lock_guard<mutex> lk(mtx_);
	return closed_;
}
template <typename T>
bool blocking_queue<T>::closed(std::unique_lock<mutex>& ) const
{
	return closed_;
}

template <typename T>
bool blocking_queue<T>::empty() const
{
	std::lock_guard<mutex> lk(mtx_);
	return empty(lk);
}
template <typename T>
bool blocking_queue<T>::full() const
{
	std::lock_guard<mutex> lk(mtx_);
	return full(lk);
}

template <typename T>
typename blocking_queue<T>::size_type blocking_queue<T>::capacity() const
{
	std::lock_guard<mutex> lk(mtx_);
	return capacity(lk);
}

template <typename T>
typename blocking_queue<T>::size_type blocking_queue<T>::size() const
{
	std::lock_guard<mutex> lk(mtx_);
	return size(lk);
}

template <typename T>
boost::queue_op_status blocking_queue<T>::try_pull_front(T& elem, std::unique_lock<mutex>& lk)
{
	if (empty(lk))
	{
		if (closed(lk)) return boost::queue_op_status::closed;
		return boost::queue_op_status::empty;
	}
	pull_front(elem, lk);
	return boost::queue_op_status::success;
}

template <typename T>
boost::queue_op_status blocking_queue<T>::try_pull_front(T& elem)
{
	std::unique_lock<mutex> lk(mtx_);
	return try_pull_front(elem, lk);
}

template <typename T>
boost::queue_op_status blocking_queue<T>::nonblocking_pull_front(T& elem)
{
	std::unique_lock<mutex> lk(mtx_);
	if (!lk.owns_lock())
	{
		return boost::queue_op_status::busy;
	}
	return try_pull_front(elem, lk);
}

template <typename T>
void blocking_queue<T>::throw_if_closed(std::unique_lock<mutex>&)
{
	if (closed_)
	{
		throw boost::sync_queue_is_closed();
	}
}

template <typename T>
void blocking_queue<T>::wait_until_not_empty(std::unique_lock<mutex>& lk)
{
	for (;;)
	{
		if (out_ != in_) break;
		throw_if_closed(lk);
		++waiting_empty_;
		not_empty_.wait(lk);
	}
}
template <typename T>
void blocking_queue<T>::wait_until_not_empty(std::unique_lock<mutex>& lk, bool & closed)
{
	for (;;)
	{
		if (out_ != in_) break;
		if (closed_) {closed=true; return;}
		++waiting_empty_;
		not_empty_.wait(lk);
	}
}

template <typename T>
void blocking_queue<T>::pull_front(T& elem)
{
	std::unique_lock<mutex> lk(mtx_);
	wait_until_not_empty(lk);
	pull_front(elem, lk);
}

// enable if T is nothrow movable
template <typename T>
T blocking_queue<T>::pull_front()
{
	std::unique_lock<mutex> lk(mtx_);
	wait_until_not_empty(lk);
	return pull_front(lk);
}

template <typename T>
boost::queue_op_status blocking_queue<T>::wait_pull_front(T& elem, std::unique_lock<mutex>& lk)
{
	if (empty(lk) && closed(lk)) {return boost::queue_op_status::closed;}
	wait_until_not_empty(lk);
	pull_front(elem, lk);
	return boost::queue_op_status::success;
}
template <typename T>
boost::queue_op_status blocking_queue<T>::wait_pull_front(T& elem)
{
	std::unique_lock<mutex> lk(mtx_);
	return wait_pull_front(elem, lk);
}

template <typename T>
boost::queue_op_status blocking_queue<T>::try_push_back(const T& elem, std::unique_lock<mutex>& lk)
{
	if (closed(lk)) return boost::queue_op_status::closed;
	size_type in_p_1 = inc(in_);
	if (in_p_1 == out_)  // full()
	{
		return boost::queue_op_status::full;
	}
	push_at(elem, in_p_1, lk);
	return boost::queue_op_status::success;
}

template <typename T>
boost::queue_op_status blocking_queue<T>::try_push_back(const T& elem)
{
	std::unique_lock<mutex> lk(mtx_);
	return try_push_back(elem, lk);
}

template <typename T>
boost::queue_op_status blocking_queue<T>::wait_push_back(const T& elem, std::unique_lock<mutex>& lk)
{
	if (closed(lk)) return boost::queue_op_status::closed;
	push_at(elem, wait_until_not_full(lk), lk);
	return boost::queue_op_status::success;
}
template <typename T>
boost::queue_op_status blocking_queue<T>::wait_push_back(const T& elem)
{
	std::unique_lock<mutex> lk(mtx_);
	return wait_push_back(elem, lk);
}


template <typename T>
boost::queue_op_status blocking_queue<T>::nonblocking_push_back(const T& elem)
{
	std::unique_lock<mutex> lk(mtx_);
	if (!lk.owns_lock()) return boost::queue_op_status::busy;
	return try_push_back(elem, lk);
}

template <typename T>
typename blocking_queue<T>::size_type blocking_queue<T>::wait_until_not_full(std::unique_lock<mutex>& lk)
{
	for (;;)
	{
		throw_if_closed(lk);
		size_type in_p_1 = inc(in_);
		if (in_p_1 != out_) // ! full()
		{
			return in_p_1;
		}
		++waiting_full_;
		not_full_.wait(lk);
	}
}

template <typename T>
void blocking_queue<T>::push_back(const T& elem)
{
	std::unique_lock<mutex> lk(mtx_);
	push_at(elem, wait_until_not_full(lk), lk);
}

template <typename T>
boost::queue_op_status blocking_queue<T>::try_push_back(T&& elem, std::unique_lock<mutex>& lk)
{
	if (closed(lk)) return boost::queue_op_status::closed;
	size_type in_p_1 = inc(in_);
	if (in_p_1 == out_) // full()
	{
		return boost::queue_op_status::full;
	}
	push_at(std::move(elem), in_p_1, lk);
	return boost::queue_op_status::success;
}
template <typename T>
boost::queue_op_status blocking_queue<T>::try_push_back(T&& elem)
{
	std::unique_lock<mutex> lk(mtx_);
	return try_push_back(std::move(elem), lk);
}

template <typename T>
boost::queue_op_status blocking_queue<T>::wait_push_back(T&& elem, std::unique_lock<mutex>& lk)
{
	if (closed(lk)) return boost::queue_op_status::closed;
	push_at(std::move(elem), wait_until_not_full(lk), lk);
	return boost::queue_op_status::success;
}
template <typename T>
boost::queue_op_status blocking_queue<T>::wait_push_back(T&& elem)
{
	std::unique_lock<mutex> lk(mtx_);
	return try_push_back(std::move(elem), lk);
}


template <typename T>
boost::queue_op_status blocking_queue<T>::nonblocking_push_back(T&& elem)
{
	std::unique_lock<mutex> lk(mtx_);
	if (!lk.owns_lock())
	{
		return boost::queue_op_status::busy;
	}
	return try_push_back(std::move(elem), lk);
}

template <typename T>
void blocking_queue<T>::push_back(T&& elem)
{
	std::unique_lock<mutex> lk(mtx_);
	push_at(std::move(elem), wait_until_not_full(lk), lk);
}

template <typename T>
blocking_message<T>& operator<<(blocking_message<T>& bm, T&& elem)
{
	bm.push(std::move(elem));
	return bm;
}

template <typename T>
blocking_message<T>& operator<<(blocking_message<T>& bm, T const&elem)
{
	bm.push(elem);
	return bm;
}

template <typename T>
blocking_message<T>& operator>>(blocking_message<T>& bm, T &elem)
{
	bm.pull(elem);
	return bm;
}

template <typename T>
blocking_queue<T>& operator<<(blocking_queue<T>& bm, T&& elem)
{
	bm.push_back(std::move(elem));
	return bm;
}

template <typename T>
blocking_queue<T>& operator<<(blocking_queue<T>& bm, T const&elem)
{
	bm.push_back(elem);
	return bm;
}

template <typename T>
blocking_queue<T>& operator>>(blocking_queue<T>& bm, T &elem)
{
	bm.pull_front(elem);
	return bm;
}

}

#endif
