#include "http_conn.h"

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站的根目录
const char* doc_root = "/home/zdb/webserver1.0/resources";

//设置文件描述符非阻塞
int setnonblocking( int fd ) {
    /*
    fcntl提供了对文件描述符的各种控制操作
    int fcntl(int fd, int cmd, ...);
    - F_GETFL：获取文件描述符的状态标志
    - F_SETFL：设置文件描述符的状态标志
    */
    int old_option = fcntl( fd, F_GETFL );
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );    //设置新状态
    return old_option;
}

// 将文件描述符fd上的EPOLLIN注册到epollfd指示的epoll内核事件表中，参数one_shot指定一个socket连接在任意时刻只能被一个线程处理
void addfd( int epollfd, int fd, bool one_shot ) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP;
    if(one_shot) {
        // 防止同一个通信被不同的线程处理
        event.events |= EPOLLONESHOT;  //EPOLLONESHOT事件使一个socket上的某个事件只能被触发一次
    }
    /*
    int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
    操作epoll的内核事件表。成功返回0,失败返回-1并设置errno
    - op参数指定操作类型：
      - EPOLL_CTL_ADD：往事件表中注册fd上的事件
      - EPOLL_CTL_MOD：修改fd上的注册事件
      - EPOLL_CTL_DEL：删除fd上的注册事件
    - fd参数：是要操作的文件描述符
    */
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 设置文件描述符非阻塞
    setnonblocking(fd);  
}

// 从epoll中移除监听的文件描述符
void removefd( int epollfd, int fd ) {
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
    close(fd);
}

// 修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}


// 所有的客户数
int http_conn::m_user_count = 0;
// 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
int http_conn::m_epollfd = -1;

// 关闭连接
void http_conn::close_conn() {
    if(m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--; // 关闭一个连接，将客户总数量-1
    }
}

// 初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in& addr){
    m_sockfd = sockfd;
    m_address = addr;
    
    // 端口复用
    int reuse = 1;
    /*
    int setsockopt(int sockfd, int level, int option_name, const void* option_value, socklen_t option_len);
    成功返回0,失败返回-1并设置errno
    level参数：指定要操作哪个协议的选项（即属性），比如IPv4,IPv6等
        - SOL_SOCKET：通用socket选项，与协议无关
    option_name参数：指定选项的名字
        - SO_REUSEADDR：重用本地地址
    */
    setsockopt( m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );

    //添加到epoll对象中
    addfd( m_epollfd, sockfd, true );   //对sockfd启用EPOLLONESHOT
    m_user_count++;  //总用户数+1
    init();
}

void http_conn::init() {
    bytes_to_send = 0;
    bytes_have_send = 0;

    m_check_state = CHECK_STATE_REQUESTLINE;  // 初始状态为检查请求行
    m_linger = false;                         // 默认不保持链接  Connection : keep-alive保持连接

    m_method = GET;                           // 默认请求方式为GET
    m_url = 0;              
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;

    //清空缓存
    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}

// 循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read() {
    //缓冲区已满
    if( m_read_idx >= READ_BUFFER_SIZE ) {
        return false;
    }

    int bytes_read = 0;  //读到的字节
    while(true) {
        // 从m_read_buf + m_read_idx索引出开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
        /*
        ssize_t recv(int sockfd, void* buf, size_t len, int flags);
        失败返回-1并设置errno
        - 读取sockfd上的数据
        - buf和len指定读缓冲区的位置和大小
        - flags为数据收发提供额外的控制
        */
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0 );
        if (bytes_read == -1) {
            if( errno == EAGAIN || errno == EWOULDBLOCK ) {
                // 没有数据
                break;
            }
            return false;   
        } else if (bytes_read == 0) {   // 对方关闭连接
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
}


// 解析一行，判断依据\r\n
http_conn::LINE_STATUS http_conn::parse_line() {
    // checked_index指向buffer（应用程序的读缓冲区）中当前正在分析的字节
    // read_index指向buffer中客户数据的尾部的下一个字节
    // buffer中第0～checked_index字节都已分析完毕，第checked_index~read_idx-1字节由下面的循环挨个分析
    char temp;
    for ( ; m_checked_idx < m_read_idx; ++m_checked_idx ) {
        temp = m_read_buf[ m_checked_idx ];    //获取当前要分析的字节
        if ( temp == '\r' ) {                  //如果当前的字节是回车符，则说明可能读取到一个完整的行
            if ( ( m_checked_idx + 1 ) == m_read_idx ) {
                /*如果该字符碰巧是目前buffer中的最后一个字节，那么这次分析没有读取到一个完整的行，
                返回LINE_OPEN以表示还需要继续读取一个buffer才能分析*/
                return LINE_OPEN;      
            } else if ( m_read_buf[ m_checked_idx + 1 ] == '\n' ) {  //\r\n表示读到一个完整的行
                m_read_buf[ m_checked_idx++ ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            //否则的话，说明客户发送的HTTP请求存在语法问题
            return LINE_BAD;
        } else if( temp == '\n' )  {  //如果当前的字节是\n，则也说明可能读取到一个完整的行
            if( ( m_checked_idx > 1) && ( m_read_buf[ m_checked_idx - 1 ] == '\r' ) ) {
                m_read_buf[ m_checked_idx-1 ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    //如果所有内容都分析完毕也没遇到\r字符，则返回LINE_OPEN，表示还需要继续读取客户数据才能进一步分析
    return LINE_OPEN;
}

// 1. 解析HTTP请求行：获得请求方法，目标URL,以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    // GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t"); // 判断第二个参数中的字符哪个在text中最先出现
    //如果请求行中没有空白字符或\t字符，则HTTP请求必有问题
    if (! m_url) { 
        return BAD_REQUEST;
    }
    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';    // 置位空字符，字符串结束符
    char* method = text;
    if ( strcasecmp(method, "GET") == 0 ) { // 忽略大小写比较
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }

    // /index.html HTTP/1.1
    // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_version = strpbrk( m_url, " \t" );
    if (!m_version) {
        return BAD_REQUEST;
    }
    // /index.html\0HTTP/1.1
    *m_version++ = '\0';
    if (strcasecmp( m_version, "HTTP/1.1") != 0 ) {
        return BAD_REQUEST;
    }

    /**
     * http://192.168.110.129:10000/index.html
    */
    if (strncasecmp(m_url, "http://", 7) == 0 ) {   
        m_url += 7;
        // 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
        m_url = strchr( m_url, '/' );   // ndex.html
    }
    if ( !m_url || m_url[0] != '/' ) {
        return BAD_REQUEST;
    }
    //HTTP请求行处理完毕，状态转移到头部字段的分析
    m_check_state = CHECK_STATE_HEADER; 
    return NO_REQUEST;
}

// 2. 解析HTTP请求头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {   
    // 遇到空行，说明得到一个正确的HTT请求
    if( text[0] == '\0' ) {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if ( m_content_length != 0 ) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    } else if ( strncasecmp( text, "Connection:", 11 ) == 0 ) {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            m_linger = true;
        }
    } else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);
    } else if ( strncasecmp( text, "Host:", 5 ) == 0 ) {
        // 处理Host头部字段
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    } else {
        //其他字段都不处理
        printf( "oop! unknow header %s\n", text );
    }
    return NO_REQUEST;
}

// 3. 我们没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content( char* text ) {
    if ( m_read_idx >= ( m_content_length + m_checked_idx ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read() {
    //初始的状态
    LINE_STATUS line_status = LINE_OK;   //记录当前行的读取状态
    HTTP_CODE ret = NO_REQUEST;          //记录HTTP请求的处理结果

    char* text = 0;
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
                || ((line_status = parse_line()) == LINE_OK)) {
        // 解析到了一行完整的数据，现在获取一行数据
        text = get_line();
        m_start_line = m_checked_idx;   //记录下一行的起始位置
        printf( "got 1 http line: %s\n", text );

        //m_check_state：当前状态机的状态
        switch ( m_check_state ) {
            case CHECK_STATE_REQUESTLINE: {   //第一个状态：解析请求行
                ret = parse_request_line( text );
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: {       //第二个状态：解析请求头部
                ret = parse_headers( text );
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                } else if ( ret == GET_REQUEST ) {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT: {      //第三个状态：解析请求内容实体
                ret = parse_content( text );
                if ( ret == GET_REQUEST ) {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default: {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}
/*当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
  如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
  映射到内存地址m_file_address处，并告诉调用者获取文件成功
*/
http_conn::HTTP_CODE http_conn::do_request()
{
    // "/home/nowcoder/webserver/resources"
    /*
    char* strcpy(char* dest, const char* src);
    把src所指向的字符串复制到dest
    */
    strcpy( m_real_file, doc_root );
    int len = strlen( doc_root );
    /*
    char* strncpy(char* dest, const char* src, size_t num);
    把src所指向的num个字符复制到dest
    */
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );
    /*
    int stat(const char* path, struct stat* buf);
    作用：获取文件信息。成功返回0,失败返回-1
    */
    if ( stat( m_real_file, &m_file_stat ) == -1 ) { //获取m_real_file文件的相关的状态信息
        return NO_RESOURCE;
    }

    // 判断是否有读的访问权限
    if ( ! ( m_file_stat.st_mode & S_IROTH ) ) {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if ( S_ISDIR( m_file_stat.st_mode ) ) {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open( m_real_file, O_RDONLY );
    /*
    创建内存映射
    void* mmap(void* start, isze_t length, int prot, int flags, int fd, off_t offset);
    mmap函数用于申请一段内存空间。我们可以将这段内存作为进程间通信的共享内存，也可以将文件直接映射到其中。
    函数成功返回0,失败返回-1并设置errno。
     - start参数允许用户使用某个特定的地址作为这段内存的起始地址，如果它被设置为NULL，则系统自动分配一个地址
     - length参数指定内存段长度
     - prot参数用来设置内存段的访问权限：
       - PROT_READ：内存段可读
       - PROT_WRITE：内存段可写
       - PROT_EXEC：内存段可执行
       - PROT_NONE：内存段不能被访问
     - flags参数控制内存段内容被修改后程序的行为
       - MAP_PRIVATE：内存段为调用进程所私有。对该内存段的修改不会反映到被映射的文件中
     - fd参数是被映射文件对应的文件描述符。它一般通过open系统调用获得
     - offset参数设置从文件的何处开始映射  
    */
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close( fd );
    return FILE_REQUEST;
}



// 对内存映射区执行munmap操作，释放内存空间
void http_conn::unmap() {
    if( m_file_address ) {
        //int munmap(void* start, isze_t length);
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}

// 写HTTP响应
bool http_conn::write() {
    int temp = 0;
    
    if ( bytes_to_send == 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        modfd( m_epollfd, m_sockfd, EPOLLIN ); 
        init();
        return true;
    }

    while(1) {
        /*
        writev函数将多块分散的内存数据一并写入文件描述符中，即集中写。失败返回-1并设置errno
        ssize_t writev(int fd, const struct iovec* vector, int count);
        - fd参数是被操作的目标文件描述符
        - vector参数类型iovec结构体描述一块内存区
        - count参数是vector数组的长度
        */
        temp = writev(m_sockfd, m_iv, m_iv_count);  // 集中写
        if ( temp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;  //已经发送的
        bytes_to_send -= temp;    //还需要发送的

        if (bytes_have_send >= m_iv[0].iov_len) {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        } else {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }

        if (bytes_to_send <= 0) { // 没有数据要发送了
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger) {  //如果长连接
                init();
                return true;
            } else {
                return false;
            }
        }
    }
}

// 往写缓冲中写入待发送的数据
bool http_conn::add_response( const char* format, ... ) {
    if( m_write_idx >= WRITE_BUFFER_SIZE ) {
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) ) {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );
    return true;
}
// 1. 响应报文状态行：协议版本，状态码，状态描述语
bool http_conn::add_status_line( int status, const char* title ) {
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}
// 2. 响应报文响应头部
bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len) {  //数据长度
    return add_response( "Content-Length: %d\r\n", content_len );
}
bool http_conn::add_content_type() {                   //数据类型
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool http_conn::add_linger() {                         //是否长连接
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}
bool http_conn::add_blank_line() {
    return add_response( "%s", "\r\n" );
}

// 3. 响应报文的正文
bool http_conn::add_content( const char* content ) {
    return add_response( "%s", content );
}


// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret) {
        case INTERNAL_ERROR:                          // 500 Internal Server Error 服务器内部错误
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:                              // 400 Bad Request 客户端请求的报文有错误
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:                             // 404 Not Found 请求的资源在服务器上没找到
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:                       // 403 Forbidden 服务器禁止访问资源
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:                            // 200 OK
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;

            bytes_to_send = m_write_idx + m_file_stat.st_size;

            return true;
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

// 由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process() {
    // 解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if ( read_ret == NO_REQUEST ) {
        modfd( m_epollfd, m_sockfd, EPOLLIN );
        return;
    }
    
    // 生成响应
    bool write_ret = process_write( read_ret );
    if ( !write_ret ) {
        close_conn();
    }
    modfd( m_epollfd, m_sockfd, EPOLLOUT );
}