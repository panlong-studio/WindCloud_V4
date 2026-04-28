#include <sys/epoll.h>
#include "epoll.h"
#include "error_check.h"

/**
 * @brief  把一个文件描述符加入 epoll 监听集合
 * @param  epfd epoll 实例 fd
 * @param  fd 要加入监听的目标 fd
 * @return 无
 */
void add_epoll_fd(int epfd,int fd){
    // evt 用来描述“我要监听什么事件”。
    struct epoll_event evt;

    // 把目标 fd 放进事件结构体里。
    evt.data.fd=fd;

    // EPOLLIN 表示监听“可读事件”。
    evt.events=EPOLLIN;

    // EPOLL_CTL_ADD 表示新增监听。
    int ret=epoll_ctl(epfd,EPOLL_CTL_ADD,fd,&evt);
    ERROR_CHECK(ret,-1,"添加 epoll 监听");
}

/**
 * @brief  把一个文件描述符从 epoll 监听集合中移除
 * @param  epfd epoll 实例 fd
 * @param  fd 要移除的目标 fd
 * @return 无
 */
void del_epoll_fd(int epfd,int fd){
    // 删除时虽然第四个参数理论上可以忽略，
    // 但这里仍然准备一个结构体，写法更统一。
    struct epoll_event evt;
    evt.data.fd=fd;
    evt.events=EPOLLIN;

    // EPOLL_CTL_DEL 表示删除监听。
    int ret=epoll_ctl(epfd,EPOLL_CTL_DEL,fd,&evt);
    ERROR_CHECK(ret,-1,"删除 epoll 监听");
}
