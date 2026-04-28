## 数据库：`netdisk_db`

以下内容基于当前第五期代码中的实际建表语句整理，代码来源见：

- [db_init.c](/home/liwenshuo/my_project/WindCloud_V4/src/server/data/db_init.c)

当前 `netdisk_db` 一共包含四张核心表：

1. `users`
2. `files`
3. `paths`
4. `file_sources`

其中：

1. `users` 负责认证
2. `files` 负责真实文件实体
3. `paths` 负责虚拟目录树
4. `file_sources` 负责第五期多点下载的数据源登记

---

## 1. 表结构总览

### 1.1 表名：`users`

| Field | Type | Null | Key | Default | Extra |
| --- | --- | --- | --- | --- | --- |
| id | int | NO | PRI | NULL | auto_increment |
| username | varchar(20) | NO | UNI | NULL |  |
| password_hash | char(64) | NO |  | NULL |  |
| salt | char(32) | NO |  | NULL |  |

### 1.2 表名：`files`

| Field | Type | Null | Key | Default | Extra |
| --- | --- | --- | --- | --- | --- |
| id | int | NO | PRI | NULL | auto_increment |
| sha256sum | binary(32) | NO | UNI | NULL |  |
| size | bigint | YES |  | NULL |  |
| count | int | NO |  | 0 |  |

### 1.3 表名：`paths`

| Field | Type | Null | Key | Default | Extra |
| --- | --- | --- | --- | --- | --- |
| id | int | NO | PRI | NULL | auto_increment |
| user_id | int | NO |  | NULL |  |
| path | varchar(255) | NO |  | NULL |  |
| file_id | int | YES |  | NULL |  |
| parent_id | int | NO | MUL | NULL |  |
| file_name | varchar(30) | NO |  | NULL |  |
| type | tinyint | NO |  | NULL |  |

补充说明：

1. `paths` 上有复合唯一键 `UNIQUE KEY (user_id, path)`
2. `paths` 上有普通索引 `INDEX idx_user_parent (user_id, parent_id)`

### 1.4 表名：`file_sources`

| Field | Type | Null | Key | Default | Extra |
| --- | --- | --- | --- | --- | --- |
| id | int | NO | PRI | NULL | auto_increment |
| file_id | int | NO | MUL | NULL |  |
| server_ip | varchar(64) | NO |  | NULL |  |
| server_port | varchar(16) | NO |  | NULL |  |
| status | tinyint | NO |  | 1 |  |

补充说明：

1. `file_sources` 上有唯一键 `UNIQUE KEY uniq_file_server (file_id, server_ip, server_port)`
2. `file_sources` 上有普通索引 `INDEX idx_file_id (file_id)`

---

## 2. 数据库整体设计说明

`netdisk_db` 的核心设计仍然是：

> 逻辑目录与真实文件分离

但到第五期为止，已经从原来的“三张表模型”扩展成“四张表模型”。

可以这样理解：

```text
用户账号
  ↓
users

用户在网盘里看到的目录和文件
  ↓
paths

这些逻辑文件真正指向的真实文件实体
  ↓
files

这些真实文件当前可以从哪些服务器下载
  ↓
file_sources
```

也就是说：

1. `users` 解决“你是谁”
2. `paths` 解决“你在网盘里看到了什么”
3. `files` 解决“这份真实内容是什么”
4. `file_sources` 解决“这份真实内容现在能从哪些服务器获取”

---

## 3. 各表详细说明

## 3.1 表：`users`（用户信息表）

该表用于存储云盘系统中注册用户的账号信息和认证数据。

### 字段说明

- `id`
  - 用户唯一标识
  - 主键
  - 自增
- `username`
  - 用户名
  - 设有唯一约束
  - 当前最大长度为 `20`
- `password_hash`
  - 用户密码的加盐哈希结果
  - 固定长度 `64`
- `salt`
  - 密码盐值
  - 固定长度 `32`

### 设计意义

当前代码不会保存明文密码，而是保存：

1. 盐值 `salt`
2. `SHA256(password + salt)` 的结果

这样可以避免直接存储明文密码。

---

## 3.2 表：`files`（真实文件表）

该表用于记录服务器真实文件实体的元数据。

当前项目真实文件统一保存在：

```text
test/server_files/<sha256>
```

`files` 表保存的就是这些真实文件的数据库信息。

### 字段说明

- `id`
  - 真实文件唯一标识
  - 主键
  - 自增
- `sha256sum`
  - 文件内容的 SHA-256 值
  - 使用 `binary(32)` 存储
  - 设有唯一约束
- `size`
  - 真实文件大小
  - 单位是字节
- `count`
  - 当前有多少个逻辑文件节点引用这份真实文件
  - 默认值为 `0`

### 设计意义

这张表是当前项目实现“秒传”和“去重”的核心。

因为：

1. 相同内容文件的 SHA-256 相同
2. `sha256sum` 唯一，所以同内容文件在 `files` 里只保留一条记录
3. 不同用户、不同目录下的多个逻辑文件，可以共同引用同一个 `file_id`

### 关于 `sha256sum` 的特别说明

当前数据库中这个字段是：

```text
binary(32)
```

因此你直接查表时看到的不是普通 64 位十六进制字符串。  
如果要查看可读的 SHA-256，通常需要这样查：

```sql
SELECT id, HEX(sha256sum) AS sha256hex, size, count
FROM files;
```

---

## 3.3 表：`paths`（逻辑路径表 / 虚拟文件系统表）

该表是整个网盘业务的核心表，用于表示：

> 某个用户在自己的网盘里看到了哪些目录和文件

它描述的是逻辑目录树，不是 Linux 真正的磁盘目录结构。

### 字段说明

- `id`
  - 当前路径节点的唯一标识
  - 主键
  - 自增
- `user_id`
  - 这条路径属于哪个用户
- `path`
  - 完整逻辑路径
  - 例如 `/doc/a.txt`
- `file_id`
  - 如果当前节点是普通文件，则指向 `files.id`
  - 如果当前节点是目录，则为 `NULL`
- `parent_id`
  - 父目录节点的 `id`
  - 根目录层通常约定为 `0`
- `file_name`
  - 用户看到的最后一级名字
  - 当前最大长度为 `30`
- `type`
  - 节点类型
  - 当前代码中：
    - `0` 表示普通文件
    - `1` 表示目录

### `type` 和 `file_id` 的关系

#### 当 `type = 0`

表示普通文件。

此时：

- `file_id` 必须指向 `files` 表中的一条真实文件记录

#### 当 `type = 1`

表示目录。

此时：

- `file_id` 应为 `NULL`

### 索引设计说明

#### `UNIQUE KEY (user_id, path)`

作用：

1. 保证同一用户不可能拥有两条完全相同的逻辑路径
2. 防止同一目录下出现重复完整路径

#### `INDEX idx_user_parent (user_id, parent_id)`

作用：

1. 当前目录列表查询是高频操作
2. `ls` 的本质就是按 `user_id + parent_id` 查子节点
3. 这个索引能明显提升列目录速度

---

## 3.4 表：`file_sources`（真实文件数据源表）

这是第五期新增的表，用于支持多点下载。

它描述的是：

> 某个真实文件当前可以从哪些服务器获取

### 字段说明

- `id`
  - 记录唯一标识
  - 主键
  - 自增
- `file_id`
  - 对应 `files.id`
  - 表示这条记录是针对哪份真实文件
- `server_ip`
  - 当前数据源服务器 IP
- `server_port`
  - 当前数据源服务器端口
- `status`
  - 当前这条数据源记录是否可用
  - 当前代码里：
    - `1` 表示可用

### 唯一键设计说明

当前唯一键为：

```text
uniq_file_server (file_id, server_ip, server_port)
```

它的作用是：

1. 同一份真实文件在同一台服务器上只保留一条记录
2. 避免重复插入相同来源

### 当前第五期如何使用这张表

#### 上传完成时

服务端会把：

1. 当前真实文件的 `file_id`
2. 当前服务端自己的 `ip`
3. 当前服务端自己的 `port`

写入 `file_sources`。

#### 控制连接申请多点下载方案时

服务端会：

1. 先根据逻辑路径查到 `file_id`
2. 再根据 `file_id` 去 `file_sources` 查有哪些可用来源
3. 把这些来源返回给客户端

#### 删除真实文件时

当某份真实文件被彻底删除时，对应的 `file_sources` 记录也应一起删除。

---

## 4. 四张表之间的关系

可以把当前数据库关系理解成下面这样：

### 4.1 `users` 与 `paths`

关系：

- 一个用户可以拥有多条路径节点

含义：

- 一个用户网盘里可以有很多文件和目录

### 4.2 `paths` 与 `files`

关系：

- 多条 `paths` 可以指向同一个 `files.id`

含义：

- 不同用户、不同目录下的逻辑文件，可以共享同一个真实文件实体

### 4.3 `files` 与 `file_sources`

关系：

- 一条 `files` 记录可以对应多条 `file_sources`

含义：

- 同一个真实文件可以存在于多台服务器上
- 第五期多点下载就是依赖这层关系

---

## 5. 当前数据库如何支撑项目功能

## 5.1 登录 / 注册

依赖：

- `users`

## 5.2 虚拟目录命令

依赖：

- `paths`

例如：

1. `pwd`
2. `cd`
3. `ls`
4. `mkdir`
5. `touch`
6. `rm`
7. `rmdir`

这些命令本质上都围绕 `paths` 展开。

## 5.3 秒传 / 去重

依赖：

- `files`

因为系统会先按 SHA-256 查 `files` 是否已有记录，再决定：

1. 直接复用已有真实文件
2. 还是继续正常上传

## 5.4 删除真实文件

依赖：

- `files.count`

只有当引用计数降到 `0` 时，服务端才会真正删除对应真实文件实体。

## 5.5 第五期多点下载

依赖：

- `file_sources`

因为控制服务器必须先知道：

1. 某份真实文件当前有哪些服务器拥有
2. 然后才能把可用数据源列表返回给客户端

---

## 6. 一句话总结

当前 `netdisk_db` 已经从原来的“三张核心表模型”扩展成“四张核心表模型”。

如果用一句话概括当前数据库设计，它可以概括成：

> `users` 负责身份认证，`paths` 负责虚拟目录树，`files` 负责真实文件实体，`file_sources` 负责第五期多点下载的数据源登记。
