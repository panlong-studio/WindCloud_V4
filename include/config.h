#ifndef __CONFIG_H__
#define __CONFIG_H__

/* 假设config.ini文件中的格式如下：key=value; */
/* 我们将key比作关键字,是我们要找的锁，用一个指针去匹配锁。 */
/* 再用一个指针去读内容 */
/* 例如调用ip,get_ip_port_mysql("ip",ip); */
/* "ip"是你要匹配的字符串，ip是取回你的数据 */
int get_target(char *key, char *value);
#endif
