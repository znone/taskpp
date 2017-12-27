# TASK
这只是个玩具！
这是一个用 C++11 写的线程池，主要特点有：
1. 任务可以让路、挂起和睡眠；
2. 任务之间可以同步，有互斥和条件变量；
3. 可以使用任务链。

### 使用方式

本项目仅由头文件组成，不需要编译。使用时需要依赖boost。

```C++
#include <task/task.h>
using namespace task;
```

#### TaskScheduler

模板类TaskScheduler提供任务调度服务，通过它创建的任务指针操纵任务。

- create_task(fun)	由一个函数创建一个任务，但不放入队列运行。
- push(fun)		把一个函数做为任务放入TaskScheduler，任务将立即排队运行。
- push(task)		把一个任务排队运行，该任务应该是由该TaskScheduler对象创建的。
- join_all()		等待所有任务运行完毕。

```C++
TaskScheduler<> scheduler;
scheduler.push([]() { 
	printf("Hello World!\n");
});
scheduler.join_all();

```

#### Task

任务由模板类Task<R>实现，模板参数R是任务的返回类型。实际使用的是它的智能指针：
```C++
template<typename R>
using TaskSharedPtr=std::shared_ptr<Task<R>>;
```

- cancel()		撤销一个任务。
- canceled()		任务是否已经被撤销。
- join()			等待任务运行完成。
- result()		获取任务的运行结果。如果任务还在运行，就等待到任务完成。如果任务被取消就抛出异常task_canceled。
- next()			指定一个任务，这个任务在本任务运行完成后才运行。
- then()			指定一个任务，这个任务在本任务运行完成后接收本任务的返回值，运行。

#### this_task

名字空间this_task里的函数只能在任务内使用。
- self()		获取当前任务自身的指针。
- yield()	当前任务让出执行。
- sleep_for(expiry_time), sleep_until(expiry_time)		让当前任务暂停一段时间。

#### 互斥和信号量

这里提供的互斥和信号量用于任务间的同步。互斥和信号量的用法与标准库类似，但是这里的信号量只能在任务内等待。
```C++
task::mutex m;
std::unique_lock<task::mutex> lk(m);
```

#### 在任务间传递消息

模板类blocking_message可以用来在任务之间传递消息。发送消息的任务会阻塞，直到有任务接收消息为止。

```C++
task::TaskScheduler<> scheduler;
task::blocking_message<std::string> message;
// send message
scheduler.push([&message]() mutable {
	message<<std::string("Hello");
});
// receive message
scheduler.push([&message]() mutable {
	std::string str;
	message>>str;
});
scheduler.join_all();
```

#### 任务链

任务链能保证任务之前的执行顺序。

```C++
task::TaskScheduler<> scheduler;
int v1=2, v2=3;
auto task1=scheduler.create_task([v1]() { 
	return v1*v1;
});
auto task2=task1->then([v2](int a) {
	return a+v2;
});
scheduler.push(task1);
```

任务task2将在任务task1执行完毕后执行。
函数pipeline可以简化任务链的创建。

#### 任务内的事件循环

可以在比较复杂的任务内使用事件循环。当没有事件时，事件循环会主动让路。当有新事件时，任务会被唤醒。
可以在任务外向事件循环投送函数。

类task::event_loop用于创建事件循环。

- post(handler)		请求事件循环执行一个函数
- stop()				停止事件循环
- run()				启动事件循环
- set_timer(expire_time, handler)
					创建定时器，请求事件循环在某个时间执行一个函数
- cancel_timer(timer_id)
					撤销定时器
- reset_timer(timer_id, expire_time)
					重置定时器的触发时间

#### 任务的堆栈

任务可以检查自身的堆栈。
- stack_size()		当前总的堆栈大小
- remaining_stack()	剩余的堆栈大小
- check_stack_overflow()
					检查堆栈是否溢出，如果溢出就终止程序

1. 固定堆栈 fixed_stack

堆栈在创建后不允许改变大小，所以必须在一开始为任务分配足够的堆栈。
这也是默认使用的堆栈。

2. 共享堆栈 shared_stack

任务需要足够多的堆栈才能正常运行，但大多空闲的时候需要的堆栈比忙碌的时候少很多。可以利用这个特性在同一个线程运行的任务间共享运行的堆栈空间，以节省内存提供更大的并发能力。当任务让路时，把堆栈数据从运行堆栈中复制出来，把运行堆栈让给其他任务使用。当任务恢复运行时，再把堆栈数据复制回去。

```C++
TaskSchedulerParam param;
param.stack_param_.init_size=4*1024;	//初始的保存堆栈数据的区域大小
param.stack_param_.capacity=1024*1024;	//实际运行的的堆栈大小

TaskScheduler<EmptyEntry, shared_stack> scheduler(param);
```

如果其它线程或任务会访问任务堆栈上的数据，则不能使用共享堆栈。

3. 不使用任务堆栈 empty_stack

这种情况下任务直接在线程上运行，任务间切换、定时器和同步等功能也不能使用。

### 性能

在 E6320 处理器上进行性能测试:

- 执行100万个简单任务，总耗时3.2秒左右，平均每个耗时3.2微秒左右。
- 同时执行1000个任务，每个任务切换10万次，总耗时8秒左右，平均每次任务切换耗时80纳秒。

### 关于测试

测试用例用到了测试框架[CppTest](https://sourceforge.net/projects/cpptest/ "CppTest")。

测试用例在 Visual Studio 2013 和 GCC 4.8 下测试通过。
