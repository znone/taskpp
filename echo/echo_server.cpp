#include <taskpp/task.h>
#include <taskpp/mutex.h>
#include <taskpp/condition_variable.h>
#include <taskpp/asio.h>

using namespace taskpp;
using namespace boost::asio;

struct session
{
	session(boost::asio::io_service& service) : socket_(service) { }
	session(const session&) = delete;
	void operator()()
	{
		char str[1024];
		boost::system::error_code ec;
		while(!ec)
		{
			size_t n=0;
			n=taskpp::read_some(socket_, buffer(str), taskpp::yield_context(ec)(boost::chrono::seconds(5))(socket_));
			if(!ec && n>0)
			{
				taskpp::write(socket_, buffer(str, n), taskpp::yield_context(ec)(boost::chrono::seconds(5))(socket_));
			}
		}
	}

	ip::tcp::socket socket_;
};

int main(int argc, char* argv[])
{
	int port=7;
	if(argc>1)
		port=atoi(argv[1]);

	taskpp::asio_task_scheduler<> scheduler;
	auto task=scheduler.push([&]() {
		ip::tcp::acceptor acceptor(taskpp::this_task::get_io_service(),
			ip::tcp::endpoint(ip::tcp::v4(), port));
		acceptor.listen();

		while(1)
		{
			std::shared_ptr<session> echo=std::make_shared<session>(acceptor.get_io_service());
			taskpp::accept(acceptor, echo->socket_);
			printf("accept %s:%u\n", 
				echo->socket_.remote_endpoint().address().to_string().data(),
				echo->socket_.remote_endpoint().port());

			taskpp::this_task::push([echo]() {
				(*echo)();
			});
		}
		printf("stop server\n");
	});
	task->join();

	return 0;
}
