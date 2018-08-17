#ifndef _PROCESSPOOL_H_ 
#define _PROCESSPOOL_H_

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <iostream>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <vector>
#include <signal.h>

#include "conn.h"
#include "fdwrapper.h"
#include "log.h"

using std::vector;

//子进程类
class process
{
public:
    process() : m_pid( -1 ) {}

public:
    int m_busy_ratio;      //给每台实际处理的服务器分配一个加权比例
    pid_t m_pid;            //目标子进程的PID
    int m_pipefd[2];        //父进程和子进程通:s用的管道
};

//第一个是conn  第三个是mgr 对链接管理的类
template < typename C, typename H, typename M > 
class processpool
{
private:
    //监听的fd 以及进程数量
    processpool (int listenfd, int process_number = 8);

public:
    //单例模式
    static processpool< C, H, M >* cteate( int listenfd, int process_number = 8 )
    {
        if ( !m_instance )
        {
            m_instance = new processpool< C, H, M >( listenfd, process_number );
        }
        return m_instance;
    }

    ~processpool()
    {
        delete[] m_sub_process;
    }

    //启动进程池
    void run( const vector<H>& arg );

private:
    void notify_parent_busy_ratio( int pipefd, M* manage );         //获取目前连接数量
    int get_most_free_srv();    //找出最空闲的服务器
    void setup_sig_pipe();     //统一事件源　
    void run_parent();
    void run_child( const vector<H>& arg );

private:
    static const int MAX_PROCESS_NUMBER = 16;       //进程池允许最大进程数量
    static const int USER_PER_PROCESS = 65536;      //每个子进程最多能处理的事件数
    static const int MAX_EVENT_NUMBER = 10000;      //epoll最多能处理的事件数目
    int m_process_number;                           //进程池中的进程总数
    int m_idx;                                      //子进程在进程池中的序号
    int m_epollfd;                                  //当前进程的epoll内核事件表fd
    int m_listenfd;                                 //监听socket
    int m_stop;                                     //子进程通过此选项来决定是否停止运行
    process* m_sub_process;                         //存储子进程的描述信息
    static processpool< C, H, M >* m_instance;      //进程池唯一实例
};

template< typename C, typename H, typename M> 
processpool< C, H, M>*processpool< C, H, M>::m_instance = NULL;

static int EPOLL_WAIT_TIME = 5000;
//父子进程通过这个管道来发送信号
static int sig_pipefd[2];

//信号处理函数 将信号发给父进程
static void sig_handler( int sig )
{
    int save_errno = errno;
    int msg = sig;
    send( sig_pipefd[1], ( char * )&msg, 1, 0 );
    errno = save_errno;
}

//添加信号 
//将sigaction中的行为添加到sig信号中
//即为sig信号设置相关的handler
static void addsig( int sig, void (handler)(int), bool restart = true )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;    //设置sa的处理函数
    if ( restart )      //重启的话 
    {
        sa.sa_flags |= SA_RESTART;  //重新调用被该信号终止的系统调用 给信号加一个重启的flag
    }
    sigfillset( &sa.sa_mask );  //将sa的掩码设置为空
    assert( sigaction( sig, &sa, NULL ) != -1 ); 
}

//构造函数 
//为每个子进程通过socketpair分配信息
//关掉父进程对管道的读端 子进程对管道的写端 即只能通过父进程向子进程发送信息
//通过socketpair创建的两个描述符 可以互相通信 即一端既可以作为服务器也可以作为客户端
template <typename C, typename H, typename M>
processpool< C, H, M>::processpool( int listenfd, int process_number ) 
    : m_listenfd( listenfd ), m_process_number( process_number ), m_idx( -1 ), m_stop( false )
{
    assert( (process_number > 0) && (process_number <= MAX_PROCESS_NUMBER) );

    m_sub_process = new process[process_number];        //存储子进程的信息
    assert( m_sub_process );
    
    for (int i = 0; i < process_number; ++i)
    {
        //将两端的sockfd建立连接
        int ret = socketpair( PF_UNIX, SOCK_STREAM, 0, m_sub_process[i].m_pipefd );
        assert( ret == 0 );
        if ( m_sub_process[i].m_pid > 0 ) //父进程
        {
            //关掉写端
            close( m_sub_process[i].m_pipefd[1] );
            m_sub_process[i].m_busy_ratio = 0;  //将其权重设置为最高
            continue;
        } 
        else //子进程 
        { 
            //关掉读端
            close( m_sub_process[i].m_pipefd[0] );
            m_idx = i;
            break;
        }
    }
}

//找到ratio最小的进程
//即权重最大
template < typename C, typename H, typename M>
int processpool< C, H, M >::get_most_free_srv()
{
    int ratio = m_sub_process[0].m_busy_ratio;
    int idx = 0;
    for (int i = 0; i < m_process_number; i++)
    {
        if (m_sub_process[i].m_busy_ratio < ratio)
        {
            idx = i;
            ratio = m_sub_process[i].m_busy_ratio;
        }
    }

    return idx;
}

//将所有的
template <typename C, typename H, typename M>
void processpool<C, H, M>::setup_sig_pipe()
{
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);
    
    //建立父子进程发送信号的管道
    int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, sig_pipefd);
    assert (ret != -1);

    //非阻塞写
    setNonBlocking(sig_pipefd[1]);  //非阻塞写
    add_read_fd( m_epollfd, sig_pipefd[0] ); //监听管道的读端并且设置为非阻塞
    
    addsig( SIGCHLD, sig_handler );     //子进程状态发生变化
    addsig( SIGTERM, sig_handler );     //终止进程
    addsig( SIGINT, sig_handler );      //键盘输入中断
    addsig( SIGPIPE, SIG_IGN );         /**往被关闭的文件描述符中写数据时护法会使程序退出
                                          SIG_IGN可以忽略 在write的时候返回-1
                                          errno设置为SIG_PIPE */
}

template<typename C, typename H, typename M>
void processpool<C, H, M>::run(const vector<H>& arg)
{
    if ( m_idx != -1 )
    {
        run_child( arg );
        return;
    }
    run_parent();
}

template <typename C, typename H, typename M>
void processpool<C, H, M>::notify_parent_busy_ratio( int pipefd, M* manager )
{
    int msg = manager->get_used_conn_cnt;
    send( pipefd, (char*)&msg, 1, 0 );
}

template<typename C, typename H, typename M>
void processpool< C, H, M>::run_child( const vector<H>& arg )
{
    setup_sig_pipe();

    //这个存放的是子进程对应的管道的读端 所以只能从这里读取
    int pipefd_read = m_sub_process[m_idx].m_pipefd[1];
    add_read_fd(m_epollfd, pipefd_read); //监听这个管道

    epoll_event events[MAX_EVENT_NUMBER];
    
    //新建一个mgr对conn的管理类
    M* manage = new M( m_epollfd, events, MAX_EVENT_NUMBER, EPOLL_WAIT_TIME );
    assert(manage);

    int number = 0;
    int ret = -1;
    
    while ( !m_stop )
    {

        //开始监听fd s
        number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, EPOLL_WAIT_TIME);
        if (number < 0 && errno != EINTR)
        {
            log(LOG_ERR, __FILE__, __LINE__, "%s", "epoll failure");
            break;
        }

        if (number == 0)
        {
            //number为0说明没有可用的描述符了
            //循环利用srv的fd
            //将m_used中的加入到m_freed中并重新建立加入到m_conn中
            manage->recycle_conns();
            continue;
        }

        //遍历所有有事件发生的fd
        for ( int i = 0; i < number; i++ )
        {
            //获得当前监听的fd
            int sockfd = events[i].data.fd;
            //如果是我们需要读进来的那个fd 也就是由父进程发送过来的fd
            if ( ( sockfd == pipefd_read ) && ( events[i].events & EPOLLIN ) )
            {
                int client = 0;
                ret = recv( sockfd, ( char * )&client, sizeof( client ), 0 );   //从sockfd中读取信息
                if ((ret < 0 && errno != EAGAIN) || ret == 0)
                {
                    continue;
                }
                else 
                {
                    //读取信息
                    //接受来自客户端的请求
                    struct sockaddr_in client_addr;
                    socklen_t client_addlen = sizeof(client_addr);
                    int connfd = accept(m_listenfd, (struct sockaddr*)&client_addr, &client_addlen );
                    if (connfd < 0)
                    {
                        log (LOG_ERR, __FILE__, __LINE__, "errno: %s", strerror(errno));
                        continue;
                    }

                    //并将接受的fd监听
                    add_read_fd( m_epollfd, connfd );
                    C* conn = manage->pick_conn(connfd);
                    if (!conn)
                    {
                        closefd(m_epollfd, connfd);
                        continue;
                    }
                    //并为这个fd初始化一个连接
                    conn->init_clt( connfd, client_addr );
                    notify_parent_busy_ratio(pipefd_read, manage);
                }
            }
            
            //处理本进程收到的信号
            else if (sockfd == sig_pipefd[0] && events[i].events & EPOLLIN)
            {
                int sig;
                char signals[1024];
                ret = recv( sig_pipefd[0], signals, sizeof(signals), 0 );
                if (ret <= 0)
                {
                    continue;
                }
                else 
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch(signals[i])
                        {
                        case SIGCHLD:
                            {
                                pid_t pid;
                                int stat;
                                //???
                                while ((pid = waitpid(-1, &stat, WNOHANG)) > 0)
                                {
                                    continue;
                                }
                                break;
                            }
                            //退出本进程
                        case SIGTERM:
                        case SIGINT:
                            {
                                m_stop = true;
                                break;
                            }
                        default:
                            {
                                break;
                            }

                        }
                    }
                }
            }
            //如果是读取
            else if (events[i].events & EPOLLIN)
            {
                //从sockfd读取
                RET_CODE result = manage->process( sockfd, READ );
                switch ( result )
                {
                case CLOSED:
                    {
                        //如果fd被关闭就通知父进程
                        notify_parent_busy_ratio(pipefd_read, manage);
                        break;
                    }
                default:
                    break;
                }
            }
            else if ( events[i].events & EPOLLOUT )
            {
                RET_CODE result = manage->process( sockfd, WRITE );
                switch(result)
                {
                case CLOSED:
                    {
                        notify_parent_busy_ratio(pipefd_read, manage);
                        break;
                    }
                default:
                    break;
                }
            }
            else 
            {
                continue;
            }

        }
    }
    close (pipefd_read);
    close (m_epollfd);
}

template <typename C, typename H, typename M>
void processpool<C, H, M>::run_parent()
{
    setup_sig_pipe();

    for (int i = 0; i < m_process_number; i++)
    {
        //将进程池中所有的父进程要写的fd全部监听
        add_read_fd(m_epollfd, m_sub_process[i].m_pipefd[0]);
    }

    //将父进程监听的这个fd加入到监听队列中
    add_read_fd(m_epollfd, m_listenfd);

    epoll_event events[MAX_EVENT_NUMBER];
    int sub_process_counter = 0;
    int new_conn = 1;
    int number = 0;
    int ret = -1;

    while (!m_stop)
    {
        number = epoll_wait( m_epollfd, events, MAX_EVENT_NUMBER, EPOLL_WAIT_TIME );
        if ((number < 0) && (errno != EINTR))
        {
            log (LOG_ERR, __FILE__, __LINE__, "%s", "epoll failure");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;
            if (sockfd == m_listenfd)
            {
                int idx = get_most_free_srv();
                send (m_sub_process[idx].m_pipefd[0], (char*)&new_conn, sizeof(new_conn), 0);
                log(LOG_INFO, __FILE__, __LINE__, "send request to child %d", idx);
            }
            //如果是来自子进程的信号
            else if (sockfd == sig_pipefd[0] && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                ret = recv(sig_pipefd[0], signals, sizeof(signals), 0);
                if (ret < 0) 
                {
                    continue;
                }
                else 
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch(signals[i])
                        {
                        case SIGCHLD:
                            pid_t pid;
                            int stat;
                            while ((pid = waitpid(-1, &stat, WNOHANG)) > 0)
                            {
                                for (int i = 0; i < m_process_number; ++i)
                                {
                                    if (m_sub_process[i].m_pid == pid)
                                    {
                                        log(LOG_INFO, __FILE__, __LINE__, "child %d join", i);
                                        close(m_sub_process[i].m_pipefd[0]);
                                        m_sub_process[i].m_pid = -1;
                                    }
                                }
                            }
                            m_stop = true;
                            for (int i = 0; i < m_process_number; i++)
                            {
                                if (m_sub_process[i].m_pid != -1)
                                {
                                        m_stop = false;
                                }
                            break;
                            }
                        case SIGTERM:
                        case SIGINT:
                            {
                                log(LOG_INFO, __FILE__, __LINE__, "%s", "kill all the child now");
                                for (int i = 0; i < m_process_number; ++i)
                                {
                                    int pid = m_sub_process[i].m_pid;
                                    if (pid != -1)
                                    {
                                        kill(pid, SIGTERM);
                                    }
                                }
                                break;
                            }
                        default:
                            {
                                break;
                            }

                        }
                    }
                }

            }
            else if (events[i].events & EPOLLIN)
            {
                int busy_ratio = 0;
                ret = recv(sockfd, (char *)&busy_ratio, sizeof(busy_ratio), 0);
                if ((ret < 0 && errno != EAGAIN) || ret == 0)
                {
                    continue;
                }
                for (int i = 0; i < m_process_number; ++i)
                {
                    if (sockfd == m_sub_process[i].m_pipefd[0])
                    {
                        m_sub_process[i].m_busy_ratio = busy_ratio;
                        break;
                    }
                }
                continue;
            }
        }
    }

    for (int i = 0; i < m_process_number; ++i)
    {
        closefd(m_epollfd, m_sub_process[i].m_pipefd[0]);
    }
    close(m_epollfd);
}


#endif
