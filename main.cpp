#define ENABLE_SDEBUG
#include "utils/screenlogger.h"
#include "utils/about.h"

#include <iostream>

#include "Sock5Server.h"

int main()
{
    std::cout << "LF::Socks5 server: " << About::ToString() << About::Version() << std::endl;
    
    Sock5Server server;
    server.Start(1234);
    server.Exec();
}