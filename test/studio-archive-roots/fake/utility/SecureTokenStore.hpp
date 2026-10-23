#pragma once
#include <string>
#include <optional>
class SecureTokenStore { public:
 static bool Save(const std::string &,const std::string &,const std::string &,std::string &);
 static std::optional<std::string> Load(const std::string &,const std::string &,std::string &);
 static bool Remove(const std::string &,const std::string &,std::string &);
};
