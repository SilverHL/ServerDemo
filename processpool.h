#ifndef _PROCESSPOOL_H_ 
#define _PROCESSPOOL_H_

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <iostream>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <vector>

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
    int m_busy_ratiol;      //给每台实际处理的服务器分配一个加权比例
    pid_t m_pid;            //目标子进程的PID
    int m_pipefd[2];        //父进程和子进程通信用的管道
};

template < typename C, typename H, typename M > 
class processpool
{
private:
    processpool (int listenfd, int process_number = 8);

public:
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
    void notify_parent_bust_ratio( int pipefd, M* manage );         //获取目前连接数量
    int get_most_free_srv();    //找出最空闲的服务器
    void setup_sig_setup();     //统一事件源　
    void run_parent();
    void run_child( const vector<H>& arg );

private:
    static const int MAX_PROCESS_NUMBER = 16;       //进程池允许最大进程数量
    static const int USER_PER_PROCESS = 65536;      //每个子进程最富哦能处理的事件数
    static const int MAX_EVENT_NUMBER = 10000;      //epoll最多能处理的事件数目
    int m_process_number;                           //进程池中的进程总数
    int m_idx;                                      //子进程在进程池中的序号
    int m_epollfd;                                  //当前进程的epoll内核事件表fd
    int m_listenfd;                                 //监听socket
    int m_stop;                                     //子进程通过此选项来决定是否停止运行
    process* m_sub_process;                         //存储子进程的描述信息
    static processpool< C, H, M >* m_instance;
};


#endif
