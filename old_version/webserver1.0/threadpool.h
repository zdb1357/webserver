#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>  //线程
#include "locker.h"

// 线程池类，将它定义为模板类是为了代码复用，模板参数T是任务类
template<typename T>
class threadpool {
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T* request);  //通过append添加任务


private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void* worker(void* arg);
    void run();    //启动线程池

    
    int m_thread_number;          // 线程的数量
    pthread_t * m_threads;        // 描述线程池的数组，大小为m_thread_number      
    int m_max_requests;           // 请求队列中最多允许的、等待处理的请求的数量  
    std::list< T* > m_workqueue;  // 请求队列
    locker m_queuelocker;         // 保护请求队列的互斥锁   
    sem m_queuestat;              // 是否有任务需要处理

    // 是否结束线程          
    bool m_stop;                    
};


//构造函数
template< typename T >
threadpool< T >::threadpool(int thread_number, int max_requests) : 
        m_thread_number(thread_number), m_max_requests(max_requests), m_stop(false), m_threads(NULL) {
    if((thread_number <= 0) || (max_requests <= 0) ) {
        throw std::exception();
    }

    m_threads = new pthread_t[m_thread_number];
    if(!m_threads) {
        throw std::exception();
    }

    /*
    int pthread_create(pthread_t* thread, const pthread_attr_t* attr, void*(*start_routine)(void*), void* arg);
    调用成功返回0，失败时返回错误码。
    - thread参数：新线程的标识符，数据类型为长整型
    - attr参数：用于设置新线程的属性，NULL表示使用默认线程属性
    */
    // 创建thread_number 个线程，并将他们设置为脱离线程。
    for ( int i = 0; i < thread_number; ++i ) {
        printf( "创建第%d个线程:\n", i);
        if(pthread_create(m_threads + i, NULL, worker, this ) != 0) {  //创建出错
            delete [] m_threads;
            throw std::exception();
        }
        //创建成功，设置线程分离
        if( pthread_detach( m_threads[i] ) ) {
            delete [] m_threads;
            throw std::exception();
        }
    }
}


//析构函数
template< typename T >
threadpool< T >::~threadpool() {
    delete [] m_threads;
    m_stop = true;
}


//往队列中添加任务
template< typename T >
bool threadpool< T >::append( T* request ) {
    // 操作工作队列时一定要加锁，因为它被所有线程共享。
    m_queuelocker.lock();
    if ( m_workqueue.size() > m_max_requests ) {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();  //信号量增加
    return true;
}


//子线程需要执行的代码  通过参数arg
template< typename T >
void* threadpool< T >::worker( void* arg ) {
    threadpool* pool = ( threadpool* )arg;
    pool->run();
    return pool;
}


template< typename T >
void threadpool< T >::run() {
    while (!m_stop) {
        m_queuestat.wait();  //取一个任务
        m_queuelocker.lock();
        if ( m_workqueue.empty() ) {
            m_queuelocker.unlock();
            continue;
        }
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if ( !request ) {     //任务为空
            continue;
        }
        request->process();  //任务的函数
    }
}

#endif
