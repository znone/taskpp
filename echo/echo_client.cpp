#include <taskpp/task.h>
#include <taskpp/mutex.h>
#include <taskpp/condition_variable.h>
#include <taskpp/asio.h>

using namespace taskpp;
using namespace boost::asio;

int main(int argc, char* argv[])
{
	std::string server_address="127.0.0.1";
	int server_port=7;
	if(argc>1)
		server_address=argv[1];
	if(argc>2)
		server_port=atoi(argv[2]);

	taskpp::asio_task_scheduler<> scheduler;
	std::atomic<size_t> n(0);
	auto task=scheduler.push([&]() {
		std::vector<char> data(1024);
		ip::tcp::socket socket(taskpp::this_task::get_io_service());
		try
		{
			taskpp::coonnect(socket, ip::tcp::endpoint(ip::address::from_string(server_address), server_port));
			while(1)
			{
				taskpp::write(socket, buffer(data));
				taskpp::read(socket, buffer(data));
				++n;
			}
		}
		catch (boost::system::system_error& e)
		{
			taskpp::this_task::get_io_service().stop();
			fprintf(stderr, "socket error(%d): %s\n", e.code().value(), e.what());
		}
	});
	while(!task->finished())
	{
		size_t n1=n;
		std::this_thread::sleep_for(std::chrono::seconds(1));
		size_t n2=n;
		printf("pingpong %lu per second\n", n2-n1);
	}
	task->join();
	return 0;
}
