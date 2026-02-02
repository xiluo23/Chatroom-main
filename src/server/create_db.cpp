#include <mysql/mysql.h>
#include <iostream>
#include <string>
#include"MyDb.h"
//只执行一次即可


void execute_sql(MYSQL *conn, const std::string &sql) {
    if (mysql_query(conn, sql.c_str())) {
        std::cerr << "Error executing SQL: " << mysql_error(conn) << std::endl;
    } else {
        std::cout << "SQL executed successfully.\n";
    }
}


