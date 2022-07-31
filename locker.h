#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>  //信号量需要

// 线程同步机制封装类

// 互斥锁类
class locker {
public:
    /*
    int pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* mutexattr);
    初始化互斥锁
    第一个参数指向要操作的目标互斥锁
    mutexattr：指定互斥锁的属性。设置NULL则表示使用默认属性
    */
    locker() {
        if(pthread_mutex_init(&m_mutex, NULL) != 0) {
            throw std::exception();
        }
    }

    ~locker() {
        pthread_mutex_destroy(&m_mutex);
    }

    bool lock() {
        return pthread_mutex_lock(&m_mutex) == 0;
    }

    bool unlock() {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }

    pthread_mutex_t *get() {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;
};


// 条件变量类
class cond {
public:
    cond(){
        if (pthread_cond_init(&m_cond, NULL) != 0) {  //成功返回0,失败返回-1并设置errno
            throw std::exception();
        }
    }
    ~cond() {
        pthread_cond_destroy(&m_cond);
    }

    //条件变量需要配合互斥锁使用
    bool wait(pthread_mutex_t *m_mutex) {
        return pthread_cond_wait(&m_cond, m_mutex) == 0;
    }
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t) {
        return pthread_cond_timedwait(&m_cond, m_mutex, &t) == 0;
    }

    //唤醒一个线程
    bool signal() {
        return pthread_cond_signal(&m_cond) == 0;
    }
    //唤醒所有线程
    bool broadcast() {
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    pthread_cond_t m_cond;
};


// 信号量类
class sem {
public:
    /*
    int sem_init(sem_t* sem, int pshared, unsigned int value);
    初始化一个未命名的POSIX信号量。成功返回0,失败返回-1并设置errno
    pshared：指定信号量的类型。如果值为0，就表示这个信号量是当前进程的局部信号量
    value:信号量的初始值

    int sem_destory(sem_t* sem);
    销毁信号量，已释放其占用的内核资源。如果销毁一个正被其他线程等待的信号量，则将导致不可预期的结果。

    int sem_wait(sem_t* sem);
    以原子操作的方式将信号量的值减1。如果信号量值为0，则sem_wait将被阻塞，直到这个信号量具有非零值。

    int sem_port(sem_t* sem);
    以原子操作的方式将信号量的值+1。当信号量的值>0时，其他正在调用sem_wait等待信号量的线程将被唤醒。
    */
    sem() {
        if( sem_init( &m_sem, 0, 0 ) != 0 ) {  
            throw std::exception();
        }
    }
    sem(int num) {
        if( sem_init( &m_sem, 0, num ) != 0 ) {
            throw std::exception();
        }
    }
    ~sem() {
        sem_destroy( &m_sem );
    }
    // 等待信号量
    bool wait() {
        return sem_wait( &m_sem ) == 0;
    }
    // 增加信号量
    bool post() {
        return sem_post( &m_sem ) == 0;
    }
private:
    sem_t m_sem;
};

#endif