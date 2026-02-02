#pragma once
#include<string>
#include<mysql/mysql.h>
#include<iostream>
#include<vector>
#include"Logger.h"
#include"ErrorCode.h"
using namespace std;
typedef unsigned long long ull;

class MyDb{
private:
    MYSQL*mysql;
public:
    MyDb();
    ~MyDb();
    bool initDB(string host,string user,string pwd,string db_name,int port);
    bool exeSQL(string sql);
    bool select_one_SQL(string sql,string& str);
    bool select_many_SQL(string sql,string& str);
    int get_id(const char* name);
    string get_name(int user_id);
};


