#include "test.h"
#include <fstream>
#include <array>
#include <iomanip>
#include <taskpp/task.h>
#include <taskpp/mutex.h>
#include <taskpp/condition_variable.h>
#include <taskpp/blocking_queue.h>
#include <taskpp/event_loop.h>
#include <taskpp/shared_stack.h>
#include <taskpp/call_in_new_stack.h>

using namespace std;
using namespace taskpp;

TestTask::TestTask()
{
	TEST_ADD(TestTask::test_simple)
	TEST_ADD(TestTask::test_chain)
	TEST_ADD(TestTask::test_pipeline)
	TEST_ADD(TestTask::test_merge)
	TEST_ADD(TestTask::test_sleep)
	TEST_ADD(TestTask::test_proirity)
	TEST_ADD(TestTask::test_mutex)
	TEST_ADD(TestTask::test_shared_stack)
	TEST_ADD(TestTask::test_call_in_new_stack)
	TEST_ADD(TestTask::test_condition_variable)
	TEST_ADD(TestTask::test_blocking_message)
	TEST_ADD(TestTask::test_cancel)
	TEST_ADD(TestTask::test_timer)
#ifndef _DEBUG
	TEST_ADD(TestTask::test_create)
	TEST_ADD(TestTask::test_yield)
#if defined(_M_X64) || defined(__x86_64__)
	TEST_ADD(TestTask::test_concurrency)
#endif
#endif
}

void TestTask::test_simple()
{
	task_scheduler<> scheduler;
	int arg=0;
	int result=scheduler.push([arg]() { 
		return arg*arg; 
	})->result();
	TEST_ASSERT_EQUALS(arg*arg, result);
}

void TestTask::test_chain()
{
	task_scheduler<> scheduler;
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
	task_scheduler<> scheduler;
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
	task_scheduler_param config;
	config.max_threads_=1;
	task_scheduler<> scheduler(config);
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
	task_scheduler<> scheduler;
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
	task_scheduler_param config;
	config.max_threads_=1;
	task_scheduler<> scheduler(config);
	taskpp::mutex m;
	string str;
	scheduler.push([&m, &str] () mutable {
		std::unique_lock<taskpp::mutex> lk(m);
		str+="first task.";
	});
	scheduler.push([&m, &str] () mutable {
		std::unique_lock<taskpp::mutex> lk(m);
		str+="second task.";
	});
	scheduler.join_all();
	TEST_ASSERT_EQUALS_OBJ("first task.second task.", str);
}

void TestTask::test_condition_variable()
{
	task_scheduler_param config;
	config.max_threads_=1;
	task_scheduler<> scheduler(config);
	taskpp::mutex m;
	taskpp::condition_variable cv;
	string str;
	scheduler.push([&m, &cv, &str] () mutable {
		std::unique_lock<taskpp::mutex> lk(m);
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
	task_scheduler<> scheduler;
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
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	scheduler.join_all();
	TEST_ASSERT_EQUALS_OBJ(send_msg, recv_msg);
}

void TestTask::test_proirity()
{
	task_scheduler_param config;
	config.max_threads_=1;
	priority_task_scheduler<> scheduler(config);

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
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	scheduler.join_all();
	
	vector<int> r {10, 4, 1};
	TEST_ASSERT_EQUALS_OBJ(r, v);
}

void TestTask::test_cancel()
{
	task_scheduler<> scheduler;
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
	catch (taskpp::task_canceled&)
	{
		puts("task is canceled.\n");	
	}
	TEST_ASSERT_EQUALS(1, a);
}

void TestTask::test_create()
{
	task_scheduler<> scheduler;
	const uint64_t n = 1000000;
	atomic<size_t> c(0);
	chrono::high_resolution_clock::time_point begin=chrono::high_resolution_clock::now();
	for(size_t i=0; i!=n; i++)
		scheduler.push([&c]() { ++c;  return log(sin(0.5)); }, 0);
	while(c!=n)
		this_thread::sleep_for(chrono::milliseconds(100));
	scheduler.join_all();
	chrono::high_resolution_clock::time_point end = chrono::high_resolution_clock::now();
	chrono::high_resolution_clock::duration lost = end - begin;
	TEST_ASSERT_EQUALS_OBJ(n, c);
	cout << "running " << n << " tasks cost time:" << lost.count() << "ns.";
	cout << " each task cost time: " << lost.count()/n << "ns." << endl;
}

void TestTask::test_yield()
{
	task_scheduler<> scheduler;
	const uint64_t n = 1000;
	const uint64_t m =100000;
	atomic<int64_t> c(0), t(0);
	chrono::high_resolution_clock::time_point begin=chrono::high_resolution_clock::now();
	for(int64_t i=0; i!=n; i++)
		scheduler.push([m, &c, &t]() { 
			for(size_t i=0; i!=m; i++)
			{
				++c;
				this_task::yield();
			}
			++t;
	});
	while(t!=n)
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	scheduler.join_all();
	chrono::high_resolution_clock::time_point end = chrono::high_resolution_clock::now();
	chrono::high_resolution_clock::duration lost = end - begin;
	TEST_ASSERT_EQUALS_OBJ(n*m, c);
	cout << "switch " << n << " tasks " << m << " times cost time:" << lost.count() << "ns.";
	cout << " each switch cost time: " << lost.count()/(n*m) << "ns." << endl;
}

void TestTask::test_timer()
{
	task_scheduler<> scheduler;
	size_t count=5;
	scheduler.push([&count]() {
		event_loop loop;
		int timer_id;
		timer_id=loop.set_timer(boost::chrono::seconds(1), true, [&]() mutable {
			time_t t=boost::chrono::system_clock::to_time_t(boost::chrono::system_clock::now());
			char str[1024];
			strftime(str, 1024, "%c", localtime(&t));
			cout << str << endl;
			--count;
			if(count==0)
			{
				loop.cancel_timer(timer_id);
				loop.stop();
			}
		});
		loop.run();
	});
	scheduler.join_all();
	TEST_ASSERT_EQUALS_OBJ(count, 0);
}

static void print_stack_info()
{
	cout<<  "stack size: " << this_task::stack_size() 
		<< ", remaining " << this_task::remaining_stack() << endl;
}

static int recfun(int n)
{
	char buffer[1024];
	memset(buffer, 'A', 1024);
	print_stack_info();
	this_task::yield();
	if(n>0) n=recfun(n-1);
	return n;
}

void TestTask::test_shared_stack()
{
	task_scheduler_param param;
	param.stack_param_.init_size=4*1024;
	param.stack_param_.capacity=1024*1024;

	task_scheduler<empty_entry, shared_stack> scheduler(param);
	atomic<int> n(10);
	scheduler.push([&n]() {
		print_stack_info();
		n=recfun(n);
	});
	while((int)n>0)
		this_thread::sleep_for(chrono::milliseconds(100));
	scheduler.join_all();
}

void TestTask::test_concurrency()
{
	const size_t n = 1000000;
	task_scheduler_param param;
	param.max_tasks_=n;
	param.stack_param_.init_size=1024;
	param.stack_param_.capacity=1024*1024;

	task_scheduler<empty_entry, shared_stack> scheduler(param);
	atomic<size_t> counter(0);
	atomic<size_t> total_tasks(0), total_stacks(0);
	atomic<bool> stoped(false);
	thread watch([&]() {
		while(!stoped)
		{
			printf("current total tasks: %8lu, used stack: %8luMB\r", 
				(size_t)total_tasks, (size_t)total_stacks/1024/1024);
			this_thread::sleep_for(chrono::seconds(1));
		}
	});
	for(size_t i=0; i!=n; i++)
	{
		scheduler.push([&]() {
			++counter;
			++total_tasks;
			total_stacks+=this_task::stack_size();

			for(size_t i=0; i!=20; i++)
			{
				this_task::sleep_for(boost::chrono::seconds(1));
			}

			total_stacks-=this_task::stack_size();
			--total_tasks;
		});
	}
	while(counter!=n)
		this_thread::sleep_for(chrono::seconds(1));
	scheduler.join_all();
	stoped=true;
	watch.join();
}

void TestTask::test_call_in_new_stack()
{
	task_scheduler<> scheduler;
	string s="a";

	auto fun= [](const string& s) {
		char c=s.back()+1;
		string s1=s+c;
		this_task::yield();
		s1+=c+1;
		return s1;
	};
	//call in thread
	string result1=call_in_new_stack(32*1024, fun, s);

	string result2=scheduler.push([fun, s](){ 
		char c=s.back()+1;
		string s1=s+c;
		//call in task
		return call_in_new_stack(32*1024, fun, s1);
	})->result();
	TEST_ASSERT_EQUALS("abc", result1);
	TEST_ASSERT_EQUALS("abcd", result2);
}


int main(int argc, char* argv[])
{
	Test::TextOutput output(Test::TextOutput::Verbose);

	TestTask test; 
	test.run(output);
	return 0;
}
