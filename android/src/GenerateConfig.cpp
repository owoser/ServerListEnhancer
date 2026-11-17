#include "Config.h"
#include <iostream>
int main(int argc, char** argv) {
    if(argc != 2) return 2;
    const std::filesystem::path dir(argv[1]);
    std::filesystem::create_directories(dir);
    if(!pl::config::writeConfig(dir/"config.json", pl::config::defaultJson(sle::android::Config{}))) return 1;
    if(!pl::config::writeConfig(dir/"config.schema.json", pl::config::schema(sle::android::Config{}))) return 1;
    std::cout << "Android config and schema generated\n";
}
