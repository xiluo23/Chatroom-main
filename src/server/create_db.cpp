#include <mysql/mysql.h>
#include <iostream>
#include <string>
#include "chatroom/db/MyDb.h"
//只执行一次即可


void execute_sql(MYSQL *conn, const std::string &sql) {
    if (mysql_query(conn, sql.c_str())) {
        std::cerr << "Error executing SQL: " << mysql_error(conn) << std::endl;
    } else {
        std::cout << "SQL executed successfully.\n";
    }
}


int main() {
    MyDb conn;
    conn.initDB("192.168.147.130","ftpuser","926472","Chatroom",3306);
    // char username[]="axxx";
    // char new_password[]="123131";
    // char salt[]="asf13122";
    // string sql = "insert into user (user_name, password, salt) values ('" + string(username) + "', '" + new_password + "', '" + salt + "')";
    // conn.exeSQL(sql.c_str());

    MYSQL *conn = mysql_init(nullptr);
    if (!conn) {
        std::cerr << "mysql_init failed\n";
        return 1;
    }

    // 连接 MySQL 
    if (!mysql_real_connect(conn, "192.168.147.130", "ftpuser", "926472", nullptr, 3306, nullptr, 0)) {
        std::cerr << "mysql_real_connect failed: " << mysql_error(conn) << std::endl;
        return 1;
    }


    创建数据库
    execute_sql(conn, "CREATE DATABASE IF NOT EXISTS Chatroom;");
    execute_sql(conn, "USE Chatroom;");



    //  创建 User 表
    execute_sql(conn, R"(
        CREATE TABLE IF NOT EXISTS user (
            user_id int PRIMARY KEY AUTO_INCREMENT,
            user_name varchar(32) NOT NULL UNIQUE,
            password varchar(64) NOT NULL,
            salt char(8) NOT NULL,
            create_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        );
    )");


    //  创建 ChatLog 表
    execute_sql(conn, R"(
        CREATE TABLE IF NOT EXISTS chat_log (
            id int PRIMARY KEY AUTO_INCREMENT,
            sender_id INT NOT NULL,
            receiver_id INT,
            is_delivered TINYINT DEFAULT 0,
            group_type ENUM('single','multi','broadcast') NOT NULL,
            content TEXT NOT NULL,
            send_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            INDEX idx_receiver(receiver_id,is_delivered),
            INDEX idx_send_time(sender_id,send_time),
            FOREIGN KEY (sender_id) REFERENCES user(user_id),
            FOREIGN KEY (receiver_id) REFERENCES user(user_id)
        );
    )");
        
   //创建User_status表
   execute_sql(conn,R"(
        CREATE TABLE IF NOT EXISTS user_status(
            user_id INT PRIMARY KEY,
            is_online TINYINT DEFAULT 0,
            last_active TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY (user_id) REFERENCES user(user_id)
        )
    )");

    mysql_close(conn);
    std::cout << "Database setup completed.\n";
    return 0;
}
