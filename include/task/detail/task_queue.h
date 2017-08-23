#ifndef _TASK_DETAIL_QUEUE_H_
#define _TASK_DETAIL_QUEUE_H_

#include <boost/thread/concurrent_queues/sync_priority_queue.hpp>
#include <boost/thread/concurrent_queues/sync_queue.hpp>
#include "devector.h"

namespace task
{

namespace detail
{

template<typename ValueType, class Container = devector<ValueType>>
class sync_queue : public boost::sync_queue<ValueType, Container>
{
	typedef boost::sync_queue<ValueType> base_;
public:
	typedef typename base_::size_type size_type;
	typedef typename base_::value_type value_type;
	typedef typename base_::time_point time_point;
	typedef typename base_::duration duration;

	sync_queue() : max_size_(SIZE_MAX) { }
	explicit sync_queue(size_t max_size) : max_size_(max_size) { }
	sync_queue(const sync_queue&) = delete;
	sync_queue& operator=(const sync_queue&) = delete;
	sync_queue(sync_queue&& src) : base_(std::move(src)), max_size_(src.max_size_) { }

	boost::queue_op_status try_push(const value_type& elem)
	{
		boost::unique_lock<boost::mutex> lk(this->mtx_);
		if(this->closed(lk))
			return boost::queue_op_status::closed;
		else if(this->data_.size()>=max_size_)
			return boost::queue_op_status::full;
		this->data_.push_back(elem);
		this->notify_not_empty_if_needed(lk);
		return boost::queue_op_status::success;
	}

	boost::queue_op_status pull_until(const time_point& tp, value_type& elem)
	{
		{
			boost::unique_lock<boost::mutex> lk(this->mtx_);
			if (boost::queue_op_status::timeout == this->wait_until_not_empty_until(lk, tp))
				return boost::queue_op_status::timeout;
		}
		return this->try_pull(elem);
	}
	boost::queue_op_status pull_for(const duration& dura, value_type& elem)
	{
		return pull_until(base_::clock::now() + dura, elem);
	}

	size_type split(sync_queue& dest, size_type remain=0, size_type n=2)
	{
		boost::unique_lock<boost::mutex> lk(this->mtx_);
		size_type size = this->data_.size();
		if (size>remain)
		{
			devector<ValueType> temp;
			size = size / n + 1;
			temp.insert(temp.end(), this->data_.begin(), this->data_.begin() + size);
			this->data_.pop_front_n(size);
			lk.unlock();
			boost::unique_lock<boost::mutex> lk2(dest.mtx_);
			dest.data_.insert(dest.data_.end(), temp.begin(), temp.end());
			dest.notify_not_empty_if_needed(lk2);
		}
		return size;
	}

	void wakeup()
	{
		boost::unique_lock<boost::mutex> lk(this->mtx_);
		this->notify_not_empty_if_needed(lk);
	}

private:
	size_t max_size_;
};

template<class ValueType,
class Container = devector<ValueType>,
class Compare = std::less<typename Container::value_type>>
class sync_priority_queue : public boost::sync_priority_queue<ValueType, Container, Compare>
{
	typedef boost::sync_queue<ValueType> base_;
public:
	typedef typename base_::size_type size_type;
	typedef typename base_::value_type value_type;
	typedef typename base_::time_point time_point;
	typedef typename base_::duration duration;

	sync_priority_queue() : max_size_(SIZE_MAX) { }
	explicit sync_priority_queue(size_t max_size) : max_size_(max_size) { }
	sync_priority_queue(const sync_priority_queue&) = delete;
	sync_priority_queue& operator=(const sync_priority_queue&) = delete;
	sync_priority_queue(sync_priority_queue&& src) : base_(std::move(src)), max_size_(src.max_size_) { }

	boost::queue_op_status try_push(const value_type& elem)
	{
		boost::unique_lock<boost::mutex> lk(this->mtx_);
		if(this->closed(lk)) return boost::queue_op_status::closed;
		else if(this->data_.size()>=max_size_) return boost::queue_op_status::full;
		this->data_.push(elem);
		this->notify_not_empty_if_needed(lk);
		return boost::queue_op_status::success;
	}

	boost::queue_op_status pull_until(const time_point& tp, value_type& elem)
	{
		{
			boost::unique_lock<boost::mutex> lk(this->mtx_);
			if (boost::queue_op_status::timeout == this->wait_until_not_empty_until(lk, tp))
				return boost::queue_op_status::timeout;
		}
		return this->try_pull(elem);
	}
	boost::queue_op_status pull_for(const duration& dura, value_type& elem)
	{
		return pull_until(base_::clock::now() + dura, elem);
	}

	size_type split(sync_priority_queue& dest, size_type remain = 0, size_type n = 2)
	{
		boost::unique_lock<boost::mutex> lk(this->mtx_);
		size_type size = this->data_.size();
		if (size>remain)
		{
			std::vector<ValueType> temp;
			size = size / n + 1;
			temp.reserve(size);
			for (size_t i = 0; i != size; i++)
			{
				temp.push_back(this->data_.pull());
			}
			lk.unlock();
			boost::unique_lock<boost::mutex> lk2(dest.mtx_);
			for (auto& v : temp)
				dest.data_.push(std::move(v));
			dest.notify_not_empty_if_needed(lk2);
		}
		return size;
	}

	void wakeup()
	{
		boost::unique_lock<boost::mutex> lk(this->mtx_);
		this->notify_not_empty_if_needed(lk);
	}

private:
	size_t max_size_;
};

}

}

#endif //_TASK_QUEUE_H_
