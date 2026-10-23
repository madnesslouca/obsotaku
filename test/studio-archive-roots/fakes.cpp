#include <OBSApp.hpp>
#include <utility/SecureTokenStore.hpp>
#include <utility/MultistreamChannelStore.hpp>
#include <util/base.h>
#include <QDir>
#include <QFileInfo>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <stdexcept>
FakeApp app;std::filesystem::path globalConfigBase;int credentialCalls=0,credentialMetadataCalls=0;
bool fakeAppAvailable=true,fakeGlobalEmpty=false;
FakeApp *App(){return fakeAppAvailable?&app:nullptr;}
int GetAppConfigPath(char *path,size_t size,const char *file){
 if(fakeGlobalEmpty){if(size)path[0]='\0';return 0;}
 if(globalConfigBase.empty()||QDir::isAbsolutePath(QString::fromUtf8(file))||QString::fromUtf8(file).contains(".."))throw std::runtime_error("unsafe fake global path");
 auto out=(globalConfigBase/std::filesystem::u8path(file)).u8string();
 if(out.size()+1>size)return -1;memcpy(path,out.c_str(),out.size()+1);return int(out.size());
}
extern "C" void blog(int,const char *format,...){va_list args;va_start(args,format);vfprintf(stderr,format,args);fputc('\n',stderr);va_end(args);}
bool SecureTokenStore::Save(const std::string&,const std::string&,const std::string&,std::string&error){++credentialCalls;error="forbidden credential IO in unencrypted fixture";return false;}
std::optional<std::string> SecureTokenStore::Load(const std::string&,const std::string&,std::string&error){++credentialCalls;error="forbidden credential IO in unencrypted fixture";return {};}
bool SecureTokenStore::Remove(const std::string&,const std::string&,std::string&error){++credentialCalls;error="forbidden credential IO in unencrypted fixture";return false;}
std::vector<MultiStreamChannel> MultistreamChannelStore::Load(){++credentialMetadataCalls;return {};}
