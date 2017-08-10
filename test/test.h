#ifndef _TEST_MYSQL_H_
#define _TEST_MYSQL_H_

#include <cpptest.h>
#include "../include/task.h"

class TestTask : public Test::Suite
{
public:
	TestTask();

private:
	void test_simple();
	void test_chain();
	void test_pipeline();
	void test_sleep();
	void test_merge();
	void test_proirity();
	void test_mutex();
	void test_condition_variable();
	void test_cancel();
	void test_performance();
	void test_yield();
};

#endif //_TEST_MYSQL_H_
