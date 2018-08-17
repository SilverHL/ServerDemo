#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>

#include "fdwrapper.h"

int setNonBlocking( int fd )
{
    int old_op = fcntl( fd, F_GETFL );
    int new_op = old_op | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_op );
    return old_op;
}

//将fd加入到epoll监听的epitem中 
//即开始监听fd
void add_read_fd( int epollfd, int fd )
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
    setNonBlocking( fd );
}

void add_write_fd( int epollfd, int fd )
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLOUT | EPOLLET;
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
    setNonBlocking( fd );
}

void closefd( int epollfd, int fd )
{
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
    close( fd );
}

void removefd( int epollfd, int fd )
{
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
}

void modfd( int epollfd, int fd, int ev )
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}
