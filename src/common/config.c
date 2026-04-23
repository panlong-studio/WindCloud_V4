#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "config.h"
#include "log.h"


/**
 * @brief  从 config.ini 中读取指定 key 对应的 value
 * @param  key 要查找的键，例如 "ip"、"port"
 * @param  value 输出参数，用来保存查找到的值
 * @return 找到返回 0，找不到或打开文件失败返回 -1
 */
int get_target(char *key, char *value) {
    static const char *config_paths[] = {
        "../config/config.ini",
        "./config/config.ini"
    };
    FILE *file = NULL;

    // 工程既可能从项目根目录启动，也可能从 bin 目录启动。
    // 因此这里顺序尝试两个相对路径，尽量兼容不同启动位置。
    for (size_t idx = 0; idx < sizeof(config_paths) / sizeof(config_paths[0]); ++idx) {
        file = fopen(config_paths[idx], "r");
        if (file != NULL) {
            break;
        }
    }

    if (file == NULL){
        LOG_WARN("未找到 config.ini 配置文件");
        return -1;
    }

    // line 用来保存每次读到的一行文本。
    char line[100];

    // fgets 每次读一行，直到读完整个文件。
    while (fgets(line, sizeof(line), file)) {
        // 先去掉行尾的 \r 或 \n，避免后面比较字符串时受到影响。
        line[strcspn(line, "\r\n")] = '\0';

        // 空行直接跳过。
        if (line[0] == '\0') {
            continue;
        }

        // # 或 ; 开头的行，当作注释行处理。
        if (line[0] == '#' || line[0] == ';') {
            continue;
        }

        // 以 = 为分隔符，把一行拆成 key 和 value 两部分。
        // 当前配置格式很简单，不支持更复杂的 section 语法。
        char *line_key = strtok(line, "=");

        // line_key 不为空，而且和目标 key 相同，才继续向下处理。
        if (line_key != NULL && strcmp(key, line_key) == 0) {
            // 继续取 = 后面的 value。
            char *line_value = strtok(NULL, "=");
            if (line_value != NULL) {
                // 把找到的 value 拷贝给调用者。
                strcpy(value, line_value);
                LOG_INFO("配置加载成功 key=%s value=%s", key, value);

                // 找到目标后就可以关文件并返回。
                fclose(file);
                return 0;
            }
        }
    }

    // 整个文件都扫完了，还没找到目标 key，也要记得关文件。
    fclose(file);
    LOG_WARN("配置项不存在 key=%s", key);
    return -1;
}
