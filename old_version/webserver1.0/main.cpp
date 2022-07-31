#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65536            // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量

// 添加文件描述符到epoll中
extern void addfd( int epollfd, int fd, bool one_shot );
// 从epoll中删除文件描述符
extern void removefd( int epollfd, int fd );

//添加信号捕捉
void addsig(int sig, void( handler )(int)){
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );  //清空
    sa.sa_handler = handler;
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}


int main( int argc, char* argv[] ) {
    //至少要传递一个端口号
    if( argc <= 1 ) {
        printf( "按照如下格式运行: %s port_number\n", basename(argv[0]));
        return 1;
    }

    //获取端口号
    int port = atoi( argv[1] );
    //对SIGPIE信号进行处理
    addsig( SIGPIPE, SIG_IGN );

    //创建并初始化线程池，
    threadpool< http_conn >* pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    } catch( ... ) {
        printf("create threadpoll failure");
        return 1;
    }

    //创建一个数组 用于保存所有打客户端信息
    http_conn* users = new http_conn[ MAX_FD ];

    //1. 创建监听的套接字
    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );
    assert(listenfd >= 0);


    // 端口复用
    int reuse = 1;
    //int setsockopt(int sockfd, int level, int option_name, const void* option_value, socklen_t option_len);
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );

    //2. 绑定
    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));       //将内存块（字符串）的前n个字节清零
    address.sin_family = AF_INET;           //协议族
    address.sin_addr.s_addr = INADDR_ANY;   //IP地址  INADDR_ANY指本机的所有IP地址，0.0.0.0
    address.sin_port = htons( port );       //转换成网络端口
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    assert(ret != -1);

    //3. 监听  int listen(int sockfd, int backlog);
    //backlog: 内核监听队列打最大长度
    ret = listen( listenfd, 5 );
    assert(ret != -1);

    // 创建epoll对象，和事件数组，添加监听的文件描述符
    epoll_event events[ MAX_EVENT_NUMBER ];
    //int epoll_create(int size);
    //size参数现在并不起作用，只是给内核一个提示，告诉它事件表需要多大。
    //该函数返回的文件描述符将用作其他所有epoll系统调用的第一个参数，以指定要访问的内核事件表。
    int epollfd = epoll_create( 5 );

    // 将监听的文件描述符添加到epoll对象中
    addfd( epollfd, listenfd, false );   //对listenfd启用对sockfd启用EPOLLONESHOT
    http_conn::m_epollfd = epollfd;

    while(true) {
        //int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout);
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        
        if ( ( number < 0 ) && ( errno != EINTR ) ) {
            printf( "epoll failure\n" );
            break;
        }

        //循环遍历事件数组
        for ( int i = 0; i < number; i++ ) {
            int sockfd = events[i].data.fd;
            
            if( sockfd == listenfd ) {
                //有客户端连接进来
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                //4.接受  int accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen);
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                if ( connfd < 0 ) {
                    printf( "errno is: %d\n", errno );
                    continue;
                } 

                if( http_conn::m_user_count >= MAX_FD ) {
                    //目前连接数满了
                    //给客户端写一个信息：服务器内部正忙。
                    printf("the server is busy! The number of client connections reached the upper limit.");
                    close(connfd);
                    continue;
                }

                char ip[16] = {0};
                inet_ntop(AF_INET, &client_address.sin_addr ,ip, sizeof(ip));

                users[connfd].init( connfd, client_address);

            } else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) {
                //对象异常断开或者错误的事件发生
                users[sockfd].close_conn();
            } else if(events[i].events & EPOLLIN) {      //读事件发生
                if(users[sockfd].read()) {               //一次性把所有数据都读完
                    pool->append(users + sockfd);
                } else {
                    users[sockfd].close_conn();         //读失败
                }
            } else if( events[i].events & EPOLLOUT ) {  //写事件发生
                if( !users[sockfd].write() ) {          //写失败   
                    users[sockfd].close_conn();
                }
            }
        }
    }
    
    close( epollfd );
    close( listenfd );
    delete [] users;  //new出来的
    delete pool;
    return 0;
}