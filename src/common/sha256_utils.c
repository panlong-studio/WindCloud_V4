#include <stdio.h>
#include <string.h>
#include "sha256_utils.h"
#include "log.h"

/**
 * @brief  计算指定文件的 SHA-256 字符串
 * @param  file_path 本地文件路径
 * @param  sha256_out 输出参数，用来保存 64 位十六进制 SHA-256 字符串
 * @return 成功返回 0，失败返回 -1
 */
int get_file_sha256(const char *file_path, char *sha256_out) {
    char command[512]={0};

    // 当前实现直接调用系统自带的 sha256sum 命令。
    // 这样实现简单，也和联调测试时人工核验 hash 的方式保持一致。
    snprintf(command,sizeof(command),"sha256sum %s",file_path);
    
    FILE *pipe = popen(command, "r");
    if(pipe == NULL) {
        LOG_ERROR("执行 sha256sum 命令获取文件 %s 的 SHA256 值失败", file_path);
        return -1; 
    }

    char buf[1024]={0};

    if(fgets(buf, sizeof(buf), pipe) != NULL) {
       // sha256sum 的输出格式通常是：
       // <64位hash><空格><空格><文件名>
       // 这里只取前 64 个字符作为真正的 hash。
       strncpy(sha256_out, buf, 64);
       sha256_out[64] = '\0';
    }else{
        LOG_ERROR("读取 sha256sum 命令输出 文件 %s 的 SHA256 值失败", file_path);
        pclose(pipe);
        return -1; 
    }

    pclose(pipe);
    return 0; 
}
