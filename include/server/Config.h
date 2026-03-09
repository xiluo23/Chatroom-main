#pragma once

#include <string>
#include <unordered_map>
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>

class Config {
public:
    // 获取单例实例
    static Config* getInstance() {
        static Config instance;
        return &instance;
    }

    // 加载配置文件
    bool loadConfig(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            std::cerr << "Failed to open config file: " << filename << std::endl;
            return false;
        }

        std::string line;
        while (std::getline(file, line)) {
            // 移除行首尾空格
            trim(line);

            // 跳过空行和注释
            if (line.empty() || line[0] == '#') {
                continue;
            }

            size_t pos = line.find('=');
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                std::string value = line.substr(pos + 1);
                trim(key);
                trim(value);
                _configMap[key] = value;
            }
        }
        return true;
    }

    // 获取配置值（字符串）
    std::string getString(const std::string& key, const std::string& defaultValue = "") {
        auto it = _configMap.find(key);
        if (it != _configMap.end()) {
            return it->second;
        }
        return defaultValue;
    }

    // 获取配置值（整数）
    int getInt(const std::string& key, int defaultValue = 0) {
        auto it = _configMap.find(key);
        if (it != _configMap.end()) {
            try {
                return std::stoi(it->second);
            } catch (...) {
                return defaultValue;
            }
        }
        return defaultValue;
    }

private:
    Config() {
        if (!loadConfig("/root/Chatroom-main/src/server/server.conf")) {
            std::cerr << "Failed to load config file " << std::endl;
            // 也可以选择退出，或者使用默认值继续
        }
    }
    ~Config() {}

    std::unordered_map<std::string, std::string> _configMap;

    // 辅助函数：去除字符串首尾空格
    void trim(std::string& s) {
        if (s.empty()) return;
        s.erase(0, s.find_first_not_of(" \t\r\n"));
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
    }
};
