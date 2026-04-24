#ifndef __CONFIG_H__
#define __CONFIG_H__

/**
 * @brief  从 config.ini 中读取指定 key 对应的 value
 * @param  key 要查找的配置项名称，例如 "ip"、"port"、"db_host"
 * @param  value 输出参数，用来保存读取到的配置值
 * @return 找到返回 0，找不到或读取失败返回 -1
 */
int get_target(char *key, char *value);

#endif
