#include"MyDb.h"

string MyDb::get_name(int user_id){
    if(mysql==NULL){
        LOG_ERROR("MySQL connection not initialized",ERR_DB_CONNECTION_FAIL);
        return "";
    }
    string sql="select user_name from user where user_id="+to_string(user_id);
    if(mysql_query(mysql,sql.c_str())){
        LOG_DB_ERROR(sql,"get_name query failed",ERR_DB_QUERY_FAIL);
        return "";
    }
    MYSQL_RES* result=mysql_store_result(mysql);  // 使用局部变量
    if(!result)return "";
    MYSQL_ROW row=mysql_fetch_row(result);         // 使用局部变量
    if(!row||!row[0]){
        mysql_free_result(result);
        return "";
    }
    string name=row[0];
    mysql_free_result(result);
    return name;
}
int MyDb::get_id(const char* name){
    if(name==nullptr){
        LOG_ERROR("Username is null",ERR_DB_EXECUTE_FAIL);
        return -1;
    }
    if(mysql==NULL){
        LOG_ERROR("MySQL connection not initialized",ERR_DB_CONNECTION_FAIL);
        return -1;
    }
    string sql="select user_id from user where user_name='"+string(name)+"'";
    if(mysql_query(mysql,sql.c_str())){
        LOG_DB_ERROR(sql,"get_id query failed for username: "+string(name),ERR_DB_QUERY_FAIL);
        return -1;
    }
    MYSQL_RES* result=mysql_store_result(mysql);  // 使用局部变量
    if(!result)return -1;
    MYSQL_ROW row=mysql_fetch_row(result);         // 使用局部变量
    if(!row||!row[0]){
        mysql_free_result(result);
        return -1;
    }
    int id = atoi(row[0]);
    mysql_free_result(result);
    return id;
}


MyDb::MyDb(){
    mysql=mysql_init(NULL);
    if(mysql==NULL){
        LOG_ERROR("Failed to initialize MySQL",ERR_DB_CONNECTION_FAIL);
        exit(0);
    }

}
MyDb::~MyDb(){
    if(mysql){
        mysql_close(mysql);
    }
}

bool MyDb::initDB(string host,string user,string pwd,string db_name,int port=3306){
    mysql=mysql_real_connect(mysql,host.c_str(),user.c_str(),pwd.c_str(),db_name.c_str(),port,NULL,0);
    if(!mysql){
        LOG_ERROR("Failed to connect to database: "+string(db_name)+" on "+host,ERR_DB_CONNECTION_FAIL);
        return false;
    }
    LOG_INFO("Database connected successfully: "+string(db_name));
    return true;
}

bool MyDb::exeSQL(string sql){
    if(mysql_query(mysql,sql.c_str())){
        LOG_DB_ERROR(sql,"SQL query failed",ERR_DB_QUERY_FAIL);
        return false;
    }
    MYSQL_RES* result=mysql_store_result(mysql);  // 使用局部变量
    //select
    if(result){
        int num_fields=mysql_num_fields(result);
        ull num_rows=mysql_num_rows(result);
        for(ull i=0;i<num_rows;i++){
            MYSQL_ROW row=mysql_fetch_row(result);  // 使用局部变量
            if(!row){
                break;
            }
            for(int j=0;j<num_fields;j++){
                cout<<row[j]<<"\t\t";
            }
            cout<<'\n';
        }
        mysql_free_result(result);
    }
    //update,insert,del
    return true;
}

bool MyDb::select_one_SQL(string sql, string& str) {
    if (mysql_query(mysql, sql.c_str())) {
        LOG_DB_ERROR(sql,"select_one_SQL query failed",ERR_DB_QUERY_FAIL);
        return false;
    }

    MYSQL_RES* result = mysql_store_result(mysql);  // 使用局部变量
    if (!result) {
        LOG_DB_ERROR(sql,"Failed to fetch result",ERR_DB_QUERY_FAIL);
        return false;
    }
    MYSQL_ROW row=mysql_fetch_row(result);  // 使用局部变量
    if(!row||!row[0]){
        mysql_free_result(result);
        return false;  // 没查到数据
    }
    int num_fields=mysql_num_fields(result);
    for(int i=0;i<num_fields;i++){
        if(row[i]){
            str+=row[i];
            str+="|";
        }
        else
            break;
    }
    if(!str.empty())
        str.pop_back();
    mysql_free_result(result);
    return true;      // 查到一条
}


bool MyDb::select_many_SQL(string sql,string &str){
    if(mysql_query(mysql,sql.c_str())){
        LOG_DB_ERROR(sql,"select_many_SQL query failed",ERR_DB_QUERY_FAIL);
        return false;
    }
    MYSQL_RES* result=mysql_store_result(mysql);  // 使用局部变量
    if(result){
        int num_fields=mysql_num_fields(result);
        ull num_rows=mysql_num_rows(result);
        LOG_DEBUG("select_many_SQL fetched "+to_string(num_rows)+" rows");
        for(ull i=0;i<num_rows;i++){
            MYSQL_ROW row=mysql_fetch_row(result);  // 使用局部变量
            if(!row){
                break;
            }
            for(int j=0;j<num_fields;j++){
                if(row[j]){
                    str+=string(row[j])+" ";
                }
            }
            str+='\n';
        }
        if(!str.empty())
            str.pop_back();
    }
    else{
        LOG_DB_ERROR(sql,"Failed to fetch result",ERR_DB_QUERY_FAIL);
        return false;
    }
    mysql_free_result(result);
    return true;
}