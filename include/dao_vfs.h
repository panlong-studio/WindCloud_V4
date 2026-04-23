#ifndef _DAO_VFS_H_
#define _DAO_VFS_H_

/**
 * @brief  根据逻辑路径查询节点信息
 * @param  user_id 用户 id
 * @param  path 逻辑路径
 * @param  out_id 输出参数，返回节点 id
 * @param  out_type 输出参数，返回节点类型
 * @return 成功返回 0，失败返回 -1
 */
int dao_get_node_by_path(int user_id,const char *path,int *out_id,int *out_type);

/**
 * @brief  列出指定目录下的所有子节点名称
 * @param  user_id 用户 id
 * @param  dir_id 当前目录节点 id
 * @param  output_buf 输出参数，用来保存拼接后的目录内容
 * @return 返回拼接了多少个字符，0 表示空目录，-1 表示查询失败
 */
int dao_list_dir(int user_id,int dir_id,char*output_buf);

/**
 * @brief  在 paths 表中创建一个新节点
 * @param  user_id 用户 id
 * @param  full_path 逻辑全路径
 * @param  parent_id 父目录节点 id
 * @param  file_name 最后一级名字
 * @param  type 节点类型，0=普通文件，1=目录
 * @return 成功返回 0，失败返回 -1
 */
int dao_create_node(int user_id,const char*full_path,
                    int parent_id,const char*file_name,int type);

/**
 * @brief  在 paths 表中创建一个普通文件节点，并关联 files.file_id
 * @param  user_id 用户 id
 * @param  full_path 逻辑全路径
 * @param  parent_id 父目录节点 id
 * @param  file_name 最后一级文件名
 * @param  file_id 关联的真实文件 id
 * @return 成功返回 0，失败返回 -1
 */
int dao_create_file_node(int user_id, const char *full_path, int parent_id, const char *file_name, int file_id);

/**
 * @brief  根据逻辑路径查询普通文件节点及其 file_id
 * @param  user_id 用户 id
 * @param  path 逻辑路径
 * @param  out_node_id 输出参数，返回 paths 节点 id
 * @param  out_file_id 输出参数，返回 files 表中的真实文件 id
 * @return 成功返回 0，失败返回 -1
 */
int dao_get_file_info_by_path(int user_id, const char *path, int *out_node_id, int *out_file_id);

/**
 * @brief  判断一个目录是否为空
 * @param  user_id 用户 id
 * @param  dir_id 目录节点 id
 * @return 1 表示空目录，0 表示非空目录，-1 表示查询失败
 */
int dao_is_dir_empty(int user_id,int dir_id);

/**
 * @brief  删除一个逻辑节点
 * @param  user_id 用户 id
 * @param  node_id 节点 id
 * @return 成功返回 0，失败返回 -1
 */
int dao_delete_node(int user_id,int node_id);

#endif // _DAO_VFS_H_
