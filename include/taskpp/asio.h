#ifndef _TASKPP_ASIO_H_
#define _TASKPP_ASIO_H_

#include <taskpp/detail/asio.h>

namespace taskpp
{

typedef detail::yield_context yield_context;

template<class ThreadEntry=empty_entry, class StackAllocator=fixedsize_stack>
class asio_task_scheduler : public detail::task_scheduler <detail::sync_queue<task_ptr>, ThreadEntry, StackAllocator,
	detail::asio_thread<detail::sync_queue<task_ptr>, ThreadEntry, StackAllocator> >
{
	typedef detail::task_scheduler<detail::sync_queue<task_ptr>, ThreadEntry, StackAllocator, 
		detail::asio_thread<detail::sync_queue<task_ptr>, ThreadEntry, StackAllocator>> base_;
public:
	asio_task_scheduler()
	{
		this->param_.enable_stealing_=false;
	}
	explicit asio_task_scheduler(const task_scheduler_param& param) : base_(param) { }
};

namespace this_task
{

boost::asio::io_service& get_io_service()
{
	detail::base_asio_thread* thread = dynamic_cast<detail::base_asio_thread*>(detail::base_task_scheduler::this_thread());
	assert(thread!=nullptr);

	return thread->service();
}

}

//wrapper asio functions
// connect

template<typename Socket>
inline void coonnect(Socket& socket, const typename Socket::endpoint_type & peer_endpoint)
{
	socket.async_connect(peer_endpoint, taskpp::yield_context());
}

template<typename Socket>
inline void coonnect(Socket& socket, const typename Socket::endpoint_type & peer_endpoint, boost::system::error_code& ec)
{
	socket.async_connect(peer_endpoint, taskpp::yield_context(ec));
}

// accept

inline boost::asio::ip::tcp::socket accept(boost::asio::ip::tcp::acceptor& acceptor)
{
	boost::asio::ip::tcp::socket socket(acceptor.get_io_service());
	acceptor.async_accept(socket, taskpp::yield_context());
	return std::move(socket);
}

template<typename Acceptor, typename Socket>
inline void accept(Acceptor& acceptor, Socket& socket)
{
	acceptor.async_accept(socket, taskpp::yield_context());
}

inline boost::asio::ip::tcp::socket accept(boost::asio::ip::tcp::acceptor& acceptor, boost::system::error_code& ec)
{
	boost::asio::ip::tcp::socket socket(acceptor.get_io_service());
	acceptor.async_accept(socket, taskpp::yield_context(ec));
	return std::move(socket);
}

template<typename Acceptor, typename Socket>
inline void accept(Acceptor& acceptor, Socket& socket, boost::system::error_code& ec)
{
	acceptor.async_accept(socket, taskpp::yield_context(ec));
}

// read

template<typename Socket, typename MutableBufferSequence>
inline size_t read(Socket& socket, const MutableBufferSequence& buffers)
{
	return socket.async_receive(buffers, taskpp::yield_context());
}

template<typename Socket, typename MutableBufferSequence>
inline size_t read(Socket& socket, const MutableBufferSequence& buffers, boost::system::error_code& ec)
{
	return socket.async_receive(buffers, taskpp::yield_context(ec));
}

template<typename Socket, typename MutableBufferSequence>
inline size_t read_some(Socket& socket, const MutableBufferSequence& buffers)
{
	return socket.async_read_some(buffers, taskpp::yield_context());
}

template<typename Socket, typename MutableBufferSequence>
inline size_t read_some(Socket& socket, const MutableBufferSequence& buffers, boost::system::error_code& ec)
{
	return socket.async_read_some(buffers, taskpp::yield_context(ec));
}

template<typename Socket, typename MutableBufferSequence>
inline size_t receive_from(Socket& socket, const MutableBufferSequence & buffers, 
	typename Socket::endpoint_type& sender_endpoint)
{
	return socket.async_read_from(buffers, sender_endpoint, taskpp::yield_context());
}

template<typename Socket, typename MutableBufferSequence>
inline size_t receive_from(Socket& socket, const MutableBufferSequence & buffers, 
	typename Socket::endpoint_type& sender_endpoint, boost::system::error_code& ec)
{
	return socket.async_read_from(buffers, sender_endpoint, taskpp::yield_context(ec));
}

// write

template<typename Socket, typename MutableBufferSequence>
inline size_t write(Socket& socket, const MutableBufferSequence& buffers)
{
	return socket.async_send(buffers, taskpp::yield_context());
}

template<typename Socket, typename MutableBufferSequence>
inline size_t write(Socket& socket, const MutableBufferSequence& buffers, boost::system::error_code& ec)
{
	return socket.async_send(buffers, taskpp::yield_context(ec));
}

template<typename Socket, typename MutableBufferSequence>
inline size_t write_some(Socket& socket, const MutableBufferSequence& buffers)
{
	return socket.async_write_some(buffers, taskpp::yield_context());
}

template<typename Socket, typename MutableBufferSequence>
inline size_t write_some(Socket& socket, const MutableBufferSequence& buffers, boost::system::error_code& ec)
{
	return socket.async_write_some(buffers, taskpp::yield_context(ec));
}

template<typename Socket, typename MutableBufferSequence>
inline size_t write_to(Socket& socket, const MutableBufferSequence& buffers, 
	const typename Socket::endpoint_type& destination, boost::asio::socket_base::message_flags flags)
{
	return socket.async_write_some(buffers, taskpp::yield_context());
}

template<typename Socket, typename MutableBufferSequence>
inline size_t write_to(Socket& socket, const MutableBufferSequence& buffers,
	const typename Socket::endpoint_type& destination, boost::asio::socket_base::message_flags flags, boost::system::error_code& ec)
{
	return socket.async_write_some(buffers, taskpp::yield_context(ec));
}

}


#endif //_TASKPP_ASIO_H_
