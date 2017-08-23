#include "test.h"
#include <fstream>
#include <array>
#include <task/task.h>
#include <task/mutex.h>
#include <task/condition_variable.h>
#include <task/blocking_queue.h>

using namespace std;
using namespace task;

TestTask::TestTask()
{
	TEST_ADD(TestTask::test_simple)
	TEST_ADD(TestTask::test_chain)
	TEST_ADD(TestTask::test_pipeline)
	TEST_ADD(TestTask::test_merge)
	TEST_ADD(TestTask::test_sleep)
	TEST_ADD(TestTask::test_proirity)
	TEST_ADD(TestTask::test_mutex)
	TEST_ADD(TestTask::test_condition_variable)
	TEST_ADD(TestTask::test_blocking_message)
	TEST_ADD(TestTask::test_cancel)
	TEST_ADD(TestTask::test_performance)
	TEST_ADD(TestTask::test_yield)
}

void TestTask::test_simple()
{
	TaskScheduler<> scheduler;
	int arg=0;
	int result=scheduler.push([arg]() { 
		return arg*arg; 
	})->result();
	TEST_ASSERT_EQUALS(arg*arg, result);
}

void TestTask::test_chain()
{
	TaskScheduler<> scheduler;
	int v1=2, v2=3;
	auto task1=scheduler.create_task([v1]() { 
		return v1*v1;
	});
	auto task2=task1->then([v2](int a) {
		return a+v2;
	});
	scheduler.push(task1);
	TEST_ASSERT_EQUALS(v1*v1+v2, task2->result());
}

void TestTask::test_pipeline()
{
	TaskScheduler<> scheduler;
	int v1=2, v2=3;
	int result=pipeline(scheduler, [v1]() { 
		return v1*v1;
	}, [v2](int a) {
		return a+v2;
	})->result();
	TEST_ASSERT_EQUALS(v1*v1+v2, result);
}

void TestTask::test_sleep()
{
	TaskSchedulerParam config;
	config.max_threads_=1;
	TaskScheduler<> scheduler(config);
	scheduler.push([this]{
		boost::chrono::steady_clock::time_point begin=boost::chrono::steady_clock::now();
		boost::chrono::steady_clock::duration d=boost::chrono::seconds(1);
		this_task::sleep_for(d);
		boost::chrono::steady_clock::time_point end=boost::chrono::steady_clock::now();
		boost::chrono::steady_clock::duration r=end-begin;
		TEST_ASSERT_DELTA( r, d, boost::chrono::seconds(1));
	});
	scheduler.join_all();
}

void TestTask::test_merge()
{
	vector<int> v {1, 9, 7, 3, 5, 8, 4, 6, 2, 10 };
	TaskScheduler<> scheduler;
	scheduler.push([&]() mutable {
		auto first=v.begin();
		auto last=v.end();
		auto middle=first+(last-first)/2;
		auto t1=scheduler.push([first, middle]() {
			sort(first, middle);
		});
		auto t2=scheduler.push([middle, last]() {
			sort(middle, last);
		});
		t1->join();
		t2->join();
		inplace_merge(first, middle, last);
	})->join();	

	vector<int> r {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
	TEST_ASSERT_EQUALS_OBJ(r, v);
}

void TestTask::test_mutex()
{
	TaskSchedulerParam config;
	config.max_threads_=1;
	TaskScheduler<> scheduler(config);
	task::mutex m;
	string str;
	scheduler.push([&m, &str] () mutable {
		std::unique_lock<task::mutex> lk(m);
		str+="first task.";
	});
	scheduler.push([&m, &str] () mutable {
		std::unique_lock<task::mutex> lk(m);
		str+="second task.";
	});
	scheduler.join_all();
	TEST_ASSERT_EQUALS_OBJ("first task.second task.", str);
}

void TestTask::test_condition_variable()
{
	TaskSchedulerParam config;
	config.max_threads_=1;
	TaskScheduler<> scheduler(config);
	task::mutex m;
	task::condition_variable cv;
	string str;
	scheduler.push([&m, &cv, &str] () mutable {
		std::unique_lock<task::mutex> lk(m);
		cv.wait(lk, [&str]() { return !str.empty(); } );
		str+="second task.";
	});
		this_thread::sleep_for(chrono::seconds(1));
	cv.notify_one();
	this_thread::sleep_for(chrono::seconds(1));
	str+="first task.";
	cv.notify_one();

	scheduler.join_all();
	TEST_ASSERT_EQUALS_OBJ("first task.second task.", str);
}

void TestTask::test_blocking_message()
{
	TaskScheduler<> scheduler;
	blocking_message<std::string> message;
	std::string send_msg("message.");
	std::string recv_msg;
	for(size_t i=0; i!=5; i++)
	{
		scheduler.push([&message, &send_msg]() mutable {
			message<<send_msg;
		});
	}
	for(size_t i=0; i!=5; i++)
	{
		scheduler.push([&message, &recv_msg]() mutable {
			message>>recv_msg;
		});
	}
	scheduler.join_all();
	TEST_ASSERT_EQUALS_OBJ(send_msg, recv_msg);
}

void TestTask::test_proirity()
{
	TaskSchedulerParam config;
	config.max_threads_=1;
	PriorityTaskScheduler<> scheduler(config);

	std::mutex m;
	std::condition_variable cv;
	bool prepare=false, ready=false;
	vector<int> v;
	auto f=[&v]() mutable {
		v.push_back(this_task::self()->priority());
	};
	scheduler.push([&]() mutable {
		unique_lock<std::mutex> lk(m);
		prepare=true;
		cv.notify_one();
		cv.wait(lk, [&ready]() { return ready; } );
	}, 0);
	{
		unique_lock<std::mutex> lk(m);
		cv.wait(lk, [&prepare]() { return prepare; } );
		scheduler.push(f, 4);
		scheduler.push(f, 1);
		scheduler.push(f, 10);
		ready=true;
	}
	cv.notify_one();
	scheduler.join_all();
	
	vector<int> r {10, 4, 1};
	TEST_ASSERT_EQUALS_OBJ(r, v);
}

void TestTask::test_cancel()
{
	TaskScheduler<> scheduler;
	std::mutex m;
	std::condition_variable cv;
	int a=0;
	auto task=scheduler.push([&]() {
		++a;
		unique_lock<std::mutex> lk(m);
		cv.wait(lk, []() {
			this_task::yield(); 
			return this_task::self()->canceled(); 
		} );
		++a; //Cannot run to here
	});
	{
		unique_lock<std::mutex> lk(m);
		task->cancel();
	}
	cv.notify_one();
	try
	{
		task->result();
	}
	catch (task::task_canceled&)
	{
		puts("task is canceled.\n");	
	}
	TEST_ASSERT_EQUALS(1, a);
}

void TestTask::test_performance()
{
	TaskScheduler<> scheduler;
	const size_t n = 1000000;
	atomic<size_t> c(0);
	chrono::high_resolution_clock::time_point begin=chrono::high_resolution_clock::now();
	for(size_t i=0; i!=n; i++)
		scheduler.push([&c]() { ++c;  return log(sin(0.5)); }, 0);
	scheduler.join_all();
	chrono::high_resolution_clock::time_point end = chrono::high_resolution_clock::now();
	chrono::high_resolution_clock::duration lost = end - begin;
	TEST_ASSERT_EQUALS_OBJ(n, c);
	cout << "running " << n << " tasks cost time:" << lost.count() << "ns.";
	cout << " each task cost time: " << lost.count()/n << "ns." << endl;
}

void TestTask::test_yield()
{
	TaskScheduler<> scheduler;
	const size_t n = 1000;
	const size_t m =100000;
	atomic<size_t> c(0);
	chrono::high_resolution_clock::time_point begin=chrono::high_resolution_clock::now();
	for(size_t i=0; i!=n; i++)
		scheduler.push([m, &c]() { 
			for(size_t i=0; i!=m; i++)
			{
				++c;
				this_task::yield();
			}
	});
	scheduler.join_all();
	chrono::high_resolution_clock::time_point end = chrono::high_resolution_clock::now();
	chrono::high_resolution_clock::duration lost = end - begin;
	TEST_ASSERT_EQUALS_OBJ(n*m, c);
	cout << "switch " << n << " tasks " << m << " times cost time:" << lost.count() << "ns.";
	cout << " each switch cost time: " << lost.count()/(n*m) << "ns." << endl;
}

int main(int argc, char* argv[])
{
	Test::TextOutput output(Test::TextOutput::Verbose);

	TestTask test; 
	test.run(output);
	return 0;
}
