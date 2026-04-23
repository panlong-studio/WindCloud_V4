#ifndef _EPOLL_H_
#define _EPOLL_H_

void add_epoll_fd(int epfd,int fd);

void del_epoll_fd(int epfd,int fd);

#endif