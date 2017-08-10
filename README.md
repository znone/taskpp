# TASK
这是一个用 C++11 写的线程池，主要特点有：
1. 任务可以让路、挂起和睡眠；
2. 任务之间可以同步，有互斥和条件变量；
3. 可以使用任务链。

### 使用方式

本项目仅由头文件组成，不需要编译。使用时需要依赖boost。

```C++
#include <task.h>
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
- self()		获取任务自身的指针。
- yield()	当前任务让出执行。
- sleep_for(expiry_time), sleep_until(expiry_time)		让当前任务停止一段时间。

#### 互斥和信号量

这里提供的互斥和信号量用于任务间的同步。信号量只能在任务内等待。

#### 任务链

任务链能保证任务之前的执行顺序。

```C++
TaskScheduler<> scheduler;
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

### 性能

在 E6320 处理器（现在很难找到比这还差的机器了吧？）上进行性能测试:

- 执行100万个简单任务，总耗时4.2秒左右，平均每个耗时4.2微秒左右。
- 同时执行1000个任务，每个任务切换10万次，总耗时8秒左右，平均每次任务切换耗时80纳秒。

### 关于测试

测试用例用到了测试框架[CppTest](https://sourceforge.net/projects/cpptest/ "CppTest")。

测试用例在 Visual Studio 2013 和 GCC 4.8 下测试通过。
