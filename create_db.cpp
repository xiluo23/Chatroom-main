#include <mysql/mysql.h>
#include <iostream>
#include <string>


void execute_sql(MYSQL *conn, const std::string &sql) {
    if (mysql_query(conn, sql.c_str())) {
        std::cerr << "Error executing SQL: " << mysql_error(conn) << std::endl;
    } else {
        std::cout << "SQL executed successfully.\n";
    }
}

int main() {
    MYSQL *conn = mysql_init(nullptr);
    if (!conn) {
        std::cerr << "mysql_init failed\n";
        return 1;
    }

    if (!mysql_real_connect(conn, "127.0.0.1", "ftpuser", "926472", nullptr, 3306, nullptr, 0)) {
        std::cerr << "mysql_real_connect failed: " << mysql_error(conn) << std::endl;
        return 1;
    }

    execute_sql(conn, "CREATE DATABASE IF NOT EXISTS Chatroom;");
    execute_sql(conn, "USE Chatroom;");

    execute_sql(conn, R"(
        CREATE TABLE IF NOT EXISTS user (
            user_id INT PRIMARY KEY AUTO_INCREMENT,
            user_name VARCHAR(32) NOT NULL UNIQUE,
            password VARCHAR(64) NOT NULL,
            salt CHAR(8) NOT NULL,
            create_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        );
    )");

    execute_sql(conn, R"(
        CREATE TABLE IF NOT EXISTS user_status (
            user_id INT PRIMARY KEY,
            is_online TINYINT DEFAULT 0,
            last_active TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY (user_id) REFERENCES user(user_id)
        );
    )");

    execute_sql(conn, R"(
        CREATE TABLE IF NOT EXISTS chat_log (
            id INT PRIMARY KEY AUTO_INCREMENT,
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

    execute_sql(conn, R"(
        CREATE TABLE IF NOT EXISTS friend_relation (
            id INT PRIMARY KEY AUTO_INCREMENT,
            user_id INT NOT NULL,
            friend_id INT NOT NULL,
            status ENUM('pending', 'accepted', 'rejected', 'blocked') NOT NULL DEFAULT 'pending',
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
            UNIQUE KEY ux_user_friend (user_id, friend_id),
            INDEX idx_friend_id (friend_id),
            FOREIGN KEY (user_id) REFERENCES user(user_id),
            FOREIGN KEY (friend_id) REFERENCES user(user_id)
        );
    )");

    execute_sql(conn, R"(
        CREATE TABLE IF NOT EXISTS conversation (
            conversation_id INT PRIMARY KEY AUTO_INCREMENT,
            type ENUM('private','group','broadcast') NOT NULL DEFAULT 'private',
            name VARCHAR(100) DEFAULT NULL,
            owner_id INT NULL,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            INDEX idx_owner (owner_id),
            FOREIGN KEY (owner_id) REFERENCES user(user_id)
        );
    )");

    execute_sql(conn, R"(
        CREATE TABLE IF NOT EXISTS conversation_member (
            id INT PRIMARY KEY AUTO_INCREMENT,
            conversation_id INT NOT NULL,
            user_id INT NOT NULL,
            joined_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            role ENUM('member','admin') NOT NULL DEFAULT 'member',
            UNIQUE KEY ux_conv_user (conversation_id, user_id),
            INDEX idx_user_conv (user_id),
            FOREIGN KEY (conversation_id) REFERENCES conversation(conversation_id),
            FOREIGN KEY (user_id) REFERENCES user(user_id)
        );
    )");


    mysql_close(conn);
    std::cout << "Database setup completed.\n";
    return 0;
}
