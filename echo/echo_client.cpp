#include <taskpp/task.h>
#include <taskpp/mutex.h>
#include <taskpp/condition_variable.h>
#include <taskpp/asio.h>
#include <numeric>

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
	std::atomic<intmax_t> n(0);
	std::atomic<size_t> total_bytes(0);
	auto task=scheduler.push([&]() {
		std::vector<char> data(1024);
		ip::tcp::socket socket(taskpp::this_task::get_io_service());
		try
		{
			taskpp::coonnect(socket, ip::tcp::endpoint(ip::address::from_string(server_address), server_port),
				taskpp::yield_context()(boost::chrono::seconds(5))(socket));
			while(1)
			{
				std::iota(data.begin(), data.end(), 0);
				taskpp::write(socket, buffer(data));
				total_bytes+=taskpp::read(socket, buffer(data));
				++n;
			}
		}
		catch (boost::system::system_error& e)
		{
			taskpp::this_task::get_io_service().stop();
			fprintf(stderr, "socket error(%d): %s\n", e.code().value(), e.what());
		}
	});
	auto begin_time = std::chrono::steady_clock::now();
	while(!task->finished())
	{
		intmax_t n1=n;
		std::this_thread::sleep_for(std::chrono::seconds(1));
		intmax_t n2=n;
		printf("pingpong %llu per second\n", n2-n1);
	}
	auto end_time = std::chrono::steady_clock::now();
	std::chrono::steady_clock::duration duration=end_time-begin_time;
	double seconds=1.0*duration.count()*std::chrono::steady_clock::period::num/std::chrono::steady_clock::period::den;
	intmax_t rps=n*std::chrono::steady_clock::period::den/std::chrono::steady_clock::period::num/duration.count();
	printf("%.4f seconds, %llu qps, %.4fMB/s\n", seconds, rps, total_bytes/seconds/1024/1024);
	task->join();
	return 0;
}
