#ifndef _EPOLL_H_
#define _EPOLL_H_

/**
 * @brief  把一个 fd 加入 epoll 监听集合
 * @param  epfd epoll 实例对应的文件描述符
 * @param  fd 要加入监听的目标 fd
 * @return 无
 */
void add_epoll_fd(int epfd,int fd);

/**
 * @brief  把一个 fd 从 epoll 监听集合中移除
 * @param  epfd epoll 实例对应的文件描述符
 * @param  fd 要移除监听的目标 fd
 * @return 无
 */
void del_epoll_fd(int epfd,int fd);

#endif
