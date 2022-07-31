#include <string.h>
#include <time.h>
#include <sys/time.h>
//#include <stdarg.h>
#include "log.h"
//#include <pthread.h>
using namespace std;

int Log::m_close_log = 1; //关闭日志

Log::Log() {
    m_count = 0;          //日志行数记录初始化为0
    m_is_async = false;   //初始为同步标志
}
Log::~Log() {
    if (m_fp != NULL) {   //打开log的文件指针
        //int fclose(FILE *stream);    关闭一个文件，成功返回0
        fclose(m_fp);
    }
}

//异步需要设置阻塞队列的长度，同步不需要设置
bool Log::init(const char *file_name, int close_log, int log_buf_size, int split_lines, int max_queue_size) {
    //如果设置了max_queue_size,则设置为异步
    if (max_queue_size >= 1) {
        m_is_async = true;  //开启异步
        m_log_queue = new block_queue<string>(max_queue_size);
        pthread_t tid;
        //flush_log_thread为回调函数,这里表示创建线程异步写日志
        /*
        int pthread_create(pthread_t* thread, const pthread_attr_t* attr, void*(*start_routine)(void*), void* arg);
        调用成功返回0，失败时返回错误码。
        - thread参数：新线程的标识符，数据类型为长整型
        - attr参数：用于设置新线程的属性，NULL表示使用默认线程属性
        - start_routine和arg参数分别指定新线程将运行的函数及其参数
        */        
        pthread_create(&tid, NULL, flush_log_thread, NULL);
    }
    
    m_close_log = close_log;
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    /*
    void* memset(void* str, int c, size_t n);
    - str：要填充的内存块
    - c：填充值
    - n：填充几个
    */
    memset(m_buf, '\0', m_log_buf_size);
    m_split_lines = split_lines;

    time_t t = time(NULL);
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    
    //char* strchr(const char* str, char c);
    //函数功能：查找一个字符c在另一个字符串str中最后出现的位置，并返回。未找到则返回NULL
    const char *p = strrchr(file_name, '/');
    char log_full_name[256] = {0};

    //生成log文件完整的名
    if (p == NULL) {
        //snprintf用于将格式化的数据写入字符串
        //int snprintf(char* str, int n, char* format[,argument, ...]);
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    }
    else {
        strcpy(log_name, p + 1);                          //生成路径名
        strncpy(dir_name, file_name, p - file_name + 1);  //生成log名
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, 
                my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;
    
    m_fp = fopen(log_full_name, "a");  //追加方式写入
    if (m_fp == NULL) {                //如果写入失败
        return false;
    }

    return true;
}


void Log::write_log(int level, const char *format, ...) {
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    char s[16] = {0};
    switch (level) {
    case 0:
        strcpy(s, "[debug]:");
        break;
    case 1:
        strcpy(s, "[info]:");
        break;
    case 2:
        strcpy(s, "[warn]:");
        break;
    case 3:
        strcpy(s, "[erro]:");
        break;
    default:
        strcpy(s, "[info]:");
        break;
    }
    //写入一个log，对m_count++, m_split_lines最大行数
    m_mutex.lock();
    m_count++;

    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0) //everyday log
    {
        
        char new_log[256] = {0};
        fflush(m_fp);
        fclose(m_fp);
        char tail[16] = {0};
       
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);
       
        if (m_today != my_tm.tm_mday) {
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        }
        else {
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        }
        m_fp = fopen(new_log, "a");
    }
 
    m_mutex.unlock();

    va_list valst;
    va_start(valst, format);

    string log_str;
    m_mutex.lock();

    //写入的具体时间内容格式
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    
    int m = vsnprintf(m_buf + n, m_log_buf_size - 1, format, valst);
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;

    m_mutex.unlock();

    if (m_is_async && !m_log_queue->full()) {
        m_log_queue->push(log_str);
    }
    else {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }

    va_end(valst);
}

void Log::flush(void) {
    m_mutex.lock();
    //强制刷新写入流缓冲区
    fflush(m_fp);
    m_mutex.unlock();
}