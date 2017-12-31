#pragma once

#include <vector>

#if defined(_MSC_VER) && _MSC_VER<1900
#define noexcept throw()
#endif //_MSC_VER

namespace taskpp
{
namespace detail
{

template <class T>
class devector
{
	typedef std::vector<T> vector_type;
	vector_type data_;
	std::size_t front_index_;

public:
	typedef typename vector_type::size_type size_type;
	typedef typename vector_type::reference reference;
	typedef typename vector_type::const_reference const_reference;
	typedef typename vector_type::iterator iterator;
	typedef typename vector_type::const_iterator const_iterator;

	devector() : front_index_(0) {}
	devector(devector const& x) noexcept
		: data_(x.data_),
		front_index_(x.front_index_)
	{}
	devector(devector&& x) noexcept
		: data_(std::move(x.data_)),
		front_index_(x.front_index_)
	{
		x.front_index_ = 0;
	}

	devector& operator=(const devector& x)
	{
		if (&x != this)
		{
			data_ = x.data_;
			front_index_ = x.front_index_;
		}
		return *this;
	}

	devector& operator=(devector&& x)
	{
		if (&x != this)
		{
			data_ = x.data_;
			front_index_ = x.front_index_;
			x.front_index_ = 0;
		}
		return *this;
	}

	bool empty() const noexcept
	{
		return data_.size() == front_index_;
	}

	size_type size() const noexcept
	{
		return data_.size() - front_index_;
	}

	reference front() noexcept
	{
		return data_[front_index_];
	}

	const_reference front() const noexcept
	{
		return data_[front_index_];
	}

	reference back() noexcept
	{
		return data_.back();
	}

	const_reference back() const noexcept
	{
		return data_.back();
	}

	void push_back(const T& x)
	{
		data_.push_back(x);
	}

	void pop_front()
	{
		++front_index_;
		if (empty()) {
			data_.clear();
			front_index_ = 0;
		}
	}

	void swap(devector& x)
	{
		data_.swap(x.data_);
		std::swap(front_index_, x.front_index_);
	}

	iterator begin() { return data_.begin() + front_index_; }
	const_iterator begin() const { return data_.begin() + front_index_; }
	iterator end() { return data_.end(); }
	const_iterator end() const { return data_.end(); }

	void insert(iterator where, const_iterator first, const_iterator last)
	{
		data_.insert(where, first, last);
	}

	void pop_front_n(size_t n)
	{
		front_index_+=n;
		if (empty()) {
			data_.clear();
			front_index_ = 0;
		}
	}
};

}
}
