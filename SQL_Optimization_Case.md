# 广播业务 SQL 性能优化案例 (N+1 问题)

在面试中，这是一个非常经典的数据库性能优化案例。以下对比展示了从“循环单次查询”到“批量查询”的重构过程。

---

## 1. 调优前代码 (The "N+1" Problem)

**问题描述**：
先查询出一个用户列表（1次查询），然后在循环中为每一个用户再去查询详细信息（ID）（N次查询）。
对于 1000 个在线用户，这会导致 **1001 次** 数据库交互，严重阻塞工作线程。

```cpp
// ❌ 调优前：典型的 N+1 查询模式

// 第一步：仅获取在线用户的名字 (1次查询)
string online_names_str = "";
string sql_names = "select user_name from user_status where is_online = 1";
conn->select_many_SQL(sql_names, online_names_str);

// 解析名字列表
char* buf = strdup(online_names_str.c_str());
char* name = strtok(buf, " \n");

while(name != NULL) {
    string current_name = name;
    
    // ⚠️ 性能瓶颈：在循环中执行 SQL 查询
    // 每次循环都要进行一次网络 IO 去数据库查 ID
    // 如果有 2000 人在线，这里就是 2000 次 SQL 查询！
    int user_id = conn->get_id(current_name.c_str()); // Select id from user where name=...
    
    if (user_id != -1) {
        // ... 执行发送逻辑
    }
    
    name = strtok(NULL, " \n");
}
free(buf);
```

---

## 2. 调优后代码 (Batch Query Optimization)

**优化方案**：
利用 SQL 的 **JOIN** 能力，一次性把“用户名”和“用户ID”都查出来。
将 **N+1 次** 数据库交互降低为 **1 次**。

```cpp
// ✅ 调优后：JOIN 批量查询

// 优化查询：通过 JOIN 一次性获取用户名和 ID
// 结果格式示例： "userA 101\nuserB 102\nuserC 103"
string sql = "select user.user_name, user.user_id "
             "from user "
             "join user_status on user.user_id = user_status.user_id "
             "where is_online = 1";

string online_users = "";
// ⚡️ 只有这一次数据库交互！
if(!conn->select_many_SQL(sql, online_users)){
    return;
}

// 处理在线用户列表
char* buf = strdup(online_users.c_str());
char* saveptr = NULL;
// 使用线程安全的 strtok_r
char* user_name_token = strtok_r(buf, " \n", &saveptr);

while(user_name_token){
    // 直接从解析结果中获取 user_id，无需再次查库
    char* user_id_token = strtok_r(NULL, " \n", &saveptr);
    if(!user_id_token) break;

    string current_name = user_name_token;
    string receiver_id = user_id_token; // ID 已经在内存里了
    
    // ... 直接执行发送逻辑
    
    user_name_token = strtok_r(NULL, " \n", &saveptr);
}
free(buf);
```

## 3. 性能对比

| 指标 | 调优前 (N+1) | 调优后 (Batch) | 提升倍数 |
| :--- | :--- | :--- | :--- |
| **在线用户数** | 2000 | 2000 | - |
| **SQL 查询次数** | 2001 次 | **1 次** | **2000 倍** |
| **网络 RTT** | ~1000ms (假设0.5ms/次) | ~1ms | 显著降低延迟 |
| **数据库负载** | 高 (CPU 飙升) | 低 | 极大减轻压力 |

---

## 4. 进阶优化 (Bonus: 批量插入日志)

如果面试官追问：“即使查询优化了，你循环里还有 `INSERT` 语句写日志怎么办？那还是 N 次写操作啊。”

**回答方案**：
"是的，目前的优化解决了**读瓶颈**。对于**写瓶颈**（N次 Insert），我的下一步计划是实现 **Batch Insert**：
在内存中拼接 SQL 语句，每 100 条记录执行一次插入。"

```sql
-- 优化前：执行 100 次
INSERT INTO chat_log VALUES (...);
INSERT INTO chat_log VALUES (...);

-- 优化后：执行 1 次
INSERT INTO chat_log VALUES (...), (...), (...), ...;
```
